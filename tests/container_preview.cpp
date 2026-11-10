#include "decoded_preview.h"

#include <huxerui/huxerui.h>
#include <huxerui/media_container.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace {

constexpr VideoRectangle EditedAperture{2, 2, 316, 236};
constexpr MediaTrack::PixelAspectRatio EditedAspect{4, 3};
constexpr std::int64_t OriginalSamples = 192000;
constexpr std::int64_t EditedSamples = 24000;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

std::shared_ptr<const File> Fixture(const char* name) {
  return std::make_shared<const File>(File(MEDIA_FIXTURES).Child(name));
}

std::shared_ptr<const File> EditedClip(const File& source, const File& directory,
    MediaPresentationRange video_edit = {{1, 4}, {7, 2}}, std::optional<std::int64_t> audio_packet_end = {}) {
  auto input = Require(MediaDemuxer::Open(Require(source.OpenRandomAccess())));
  Check(input.Tracks().size() == 2, "The edited preview fixture requires two tracks");
  std::array tracks{input.Tracks()[0], input.Tracks()[1]};
  const auto index = [&](std::uint32_t id) {
    return std::size_t(std::ranges::find(tracks, id, &MediaTrack::id) - tracks.begin());
  };
  std::array<std::vector<ContainerPacket>, 2> packets;
  while (auto item = Require(input.Read())) {
    auto& packet = std::get<ContainerPacket>(*item);
    packets.at(index(packet.track_id)).push_back(std::move(packet));
  }
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    auto& track = tracks[i];
    if (track.kind == TrackKind::Video) {
      track.visible = EditedAperture;
      track.pixel_aspect_ratio = EditedAspect;
      track.presentation_edit = video_edit;
    }
    if (track.kind == TrackKind::Audio) {
      if (audio_packet_end) {
        auto& last = packets[i].back();
        const auto duration = *audio_packet_end - last.pts->Rescale(48000)->ticks;
        Check(duration > 0 && MediaTimestamp{duration, 48000}.Compare(*last.duration) < 0,
              "The tail fixture must shorten the final AAC packet");
        last.duration = MediaTimestamp{duration, 48000};
        track.padding_samples.reset();
      } else {
        track.padding_samples = track.padding_samples.value_or(0) + OriginalSamples - EditedSamples;
        track.presentation_edit->end = {EditedSamples, 48000};
      }
      packets[i].front().sync_sample = false;
      packets[i].back().sync_sample = false;
    }
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = directory.Child("media-preview-edited-" + std::to_string(now) + ".mp4");
  auto stream = Require(path.OpenRandomAccess(FileAccessMode::ReadWrite, FileOpenMode::CreateNew));
  auto file = std::shared_ptr<const File>(new File(path), [](const File* file) {
    try {
      [[maybe_unused]] const bool removed = file->Delete();
    } catch (...) {}
    delete file;
  });
  auto mux = Require(MediaMuxer::Open(std::move(stream), ContainerFormat::Mp4, tracks));
  std::array<std::size_t, 2> cursor{};
  std::array<bool, 2> ended{};
  std::optional<std::uint32_t> requested;
  while (!ended[0] || !ended[1]) {
    const auto i = requested ? index(*requested) : std::size_t(ended[0]);
    if (cursor[i] == packets[i].size()) {
      Require(mux.EndTrack(tracks[i].id));
      ended[i] = true;
      requested.reset();
    } else {
      requested = Require(mux.TryWrite(packets[i][cursor[i]]));
      if (!requested) ++cursor[i];
    }
  }
  Require(mux.Finish());
  return file;
}

struct Observations {
  std::optional<float> seek;
  std::optional<MediaTimestamp> first_frame;
  std::optional<MediaTimestamp> first_duration;
  std::optional<VideoFormat> format;
  Size size;
  float position = 0;
  float duration = 0;
  unsigned epoch = 0;
  bool paused = false;
};

TaskHandle Play(const TaskScope& tasks, std::shared_ptr<const File> file, Observations& seen, std::exception_ptr& error) {
  return tasks.Launch([file = std::move(file), &seen, &error]() -> Task<void> {
    try {
      co_await media_preview::PlayContainer(file, {
          .reset_video = [&](Size size) {
            seen.size = size;
            seen.first_frame.reset();
            ++seen.epoch;
          },
          .publish = [&](const VideoFrame& frame) {
            if (!seen.first_frame) {
              Check(frame.Validate(), "Preview published invalid frame planes");
              const auto pixels = media_preview::ConvertFramePixels(frame);
              Check(!pixels.empty(), "Preview could not convert the decoded picture");
              seen.first_frame = frame.pts;
              seen.first_duration = frame.duration;
              seen.format = frame.format;
            }
          },
          .paused = [&] { return seen.paused; },
          .take_seek = [&] { return std::exchange(seen.seek, std::nullopt); },
          .position = [&](float position, float duration) {
            seen.position = position;
            seen.duration = duration;
          },
      });
    } catch (...) { error = std::current_exception(); }
  });
}

template<class Condition>
void Until(testing::UiTest& ui, const std::exception_ptr& failure, Condition condition, const char* error) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    if (failure) std::rethrow_exception(failure);
    ui.Pump(4ms);
    std::this_thread::sleep_for(2ms);
  }
  if (failure) std::rethrow_exception(failure);
  Check(condition(), error);
}

void CheckExport(const File& source, const File& output, std::int64_t audio_samples, bool reorder = false) {
  auto input = Require(MediaDemuxer::Open(Require(source.OpenRandomAccess())));
  auto exported = Require(MediaDemuxer::Open(Require(output.OpenRead())));
  Check(exported.Format() == ContainerFormat::Mp4 && exported.Tracks().size() == 2,
        "Export is not a sequentially readable two-track MP4");
  const auto audio = std::ranges::find(exported.Tracks(), CodecId::Aac, &MediaTrack::codec);
  const auto video = std::ranges::find(exported.Tracks(), CodecId::H264, &MediaTrack::codec);
  Check(audio != exported.Tracks().end() && video != exported.Tracks().end() &&
        audio->priming_samples && audio->padding_samples, "Export lost AAC/H.264 track or trim metadata");
  const auto source_video = std::ranges::find(input.Tracks(), TrackKind::Video, &MediaTrack::kind);
  Check(source_video != input.Tracks().end() && source_video->coded_width && source_video->coded_height,
        "The source has no coded video dimensions");
  const auto expected = source_video->visible.value_or(
      VideoRectangle{0, 0, *source_video->coded_width, *source_video->coded_height});
  const auto actual = video->visible.value_or(VideoRectangle{0, 0, *video->coded_width, *video->coded_height});
  Check(actual == VideoRectangle{0, 0, expected.width, expected.height}, "Export lost the source's visible area");
  Check(video->pixel_aspect_ratio == source_video->pixel_aspect_ratio, "Export changed the display aspect ratio");
  Check(video->presentation_edit == input.PresentationRange(source_video->id) &&
        exported.PresentationRange(video->id) == input.PresentationRange(source_video->id),
        "Export changed the effective video presentation range");
  std::vector<MediaTimestamp> input_pictures, exported_pictures;
  while (auto item = Require(input.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    if (packet.track_id == video->id && packet.pts && packet.pts->ticks >= 0) input_pictures.push_back(*packet.pts);
  }
  std::int64_t coded_audio = 0;
  std::optional<MediaTimestamp> last_dts;
  bool reordered = false;
  while (auto item = Require(exported.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    if (packet.track_id == video->id) {
      Check(packet.pts && packet.dts && packet.duration, "Export lost video timing");
      if (last_dts) Check(packet.dts->Compare(*last_dts) > 0, "Export DTS moved backwards");
      if (!reorder) Check(packet.pts == packet.dts, "Export reorders its Baseline video");
      if (!exported_pictures.empty()) reordered |= packet.pts->Compare(exported_pictures.back()) < 0;
      last_dts = packet.dts;
      exported_pictures.push_back(*packet.pts);
    } else {
      coded_audio += packet.duration->Rescale(48000)->ticks;
    }
  }
  const auto earlier = [](MediaTimestamp a, MediaTimestamp b) { return a.Compare(b) < 0; };
  std::ranges::sort(input_pictures, earlier);
  std::ranges::sort(exported_pictures, earlier);
  Check(!input_pictures.empty() && input_pictures == exported_pictures, "Export changed picture presentation times");
  Check(coded_audio - std::int64_t(*audio->priming_samples + *audio->padding_samples) == audio_samples,
        "Export changed the trimmed audio length");
  if (reorder) std::cout << "VideoToolbox reordered output: " << reordered << '\n';
}

std::optional<ContainerPacket> ReadPacket(MediaDemuxer& demux) {
  while (auto item = Require(demux.Read())) {
    if (auto* packet = std::get_if<ContainerPacket>(&*item)) return std::move(*packet);
  }
  return {};
}

void CheckCopy(const File& source, const File& output, ContainerFormat format) {
  auto open = [](const File& file) { return Require(MediaDemuxer::Open(Require(file.OpenRandomAccess()))); };
  const auto original = open(source);
  const auto copy = open(output);
  Check(copy.Format() == format && copy.Tracks().size() == original.Tracks().size(), "Packet copy changed the track set");
  for (const auto& track : original.Tracks()) {
    const auto found = std::ranges::find(copy.Tracks(), track.id, &MediaTrack::id);
    Check(found != copy.Tracks().end() && found->codec == track.codec && found->sample_entry == track.sample_entry &&
          found->initialization_data == track.initialization_data && found->visible == track.visible &&
          found->rotation == track.rotation && found->pixel_aspect_ratio == track.pixel_aspect_ratio &&
          found->video_extensions == track.video_extensions && found->presentation_edit == track.presentation_edit &&
          copy.PresentationRange(track.id) == original.PresentationRange(track.id) &&
          found->priming_samples == track.priming_samples && found->padding_samples == track.padding_samples,
          "Packet copy changed the track description or presentation metadata");
    auto before = open(source), after = open(output);
    Require(before.SelectTracks(std::span(&track.id, 1)));
    Require(after.SelectTracks(std::span(&track.id, 1)));
    while (const auto expected = ReadPacket(before)) {
      const auto actual = ReadPacket(after);
      Check(actual && actual->track_id == expected->track_id && actual->pts == expected->pts &&
            actual->dts == expected->dts && actual->duration == expected->duration &&
            actual->sync_sample == expected->sync_sample &&
            std::ranges::equal(actual->data.AsBytes(), expected->data.AsBytes()), "Packet copy changed access units or timing");
    }
    Check(!ReadPacket(after), "Packet copy added access units");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    TaskScope tasks;
    const Application app([] { return View{}; }, {
        .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
    });
    testing::UiTest ui(app);
    const auto mp4 = Fixture("preview-avc-aac.mp4");
    std::exception_ptr failure;
    Observations seen;
    auto preview = Play(tasks, mp4, seen, failure);
    Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.15F; },
          "The MP4 preview did not publish video and advance its audio clock");
    for (float target : {2.1F, 0.6F, 1.45F}) {
      const auto epoch = seen.epoch;
      seen.seek = target;
      Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target + 0.02F; },
            "MP4 seek did not produce a new picture and audio clock");
      const auto first = seen.first_frame->Rescale(1000)->ticks;
      Check(first >= target * 1000 - 1 && first < target * 1000 + 150,
            "The first picture after seeking did not match the requested position");
    }
    const auto previous = seen.epoch;
    seen.paused = true;
    seen.seek = 0.9F;
    Until(ui, failure, [&] { return seen.epoch > previous && seen.first_frame; },
          "Paused seek did not publish the requested frame");
    Check(seen.first_frame->Compare({9, 10}) >= 0 && seen.position == 0.9F,
          "Paused seek advanced the playback clock or showed an earlier frame");
    preview.Cancel();
    ui.Pump();

    seen = {};
    preview = Play(tasks, Fixture("preview-hevc10.mov"), seen, failure);
    Until(ui, failure, [&] { return seen.first_frame.has_value(); }, "The Main10 MOV did not publish a picture");
    Check(seen.format->pixel_format == VideoPixelFormat::P010 && seen.format->matrix == ColorMatrix::BT709 &&
          seen.format->rotation == VideoRotation::Clockwise90 && seen.size.width == 96 && seen.size.height == 128,
          "Main10 preview lost bit depth, color, or rotation");
    preview.Cancel();
    ui.Pump();

    const File temporary(std::filesystem::temp_directory_path().u8string());
    const auto edited = EditedClip(*mp4, temporary);
    seen = {};
    preview = Play(tasks, edited, seen, failure);
    Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.15F; }, "The edited MP4 did not start playing");
    Check(seen.format->visible == EditedAperture && seen.size == Size{316.0F * 4 / 3, 236.0F} &&
          seen.first_frame->Compare({1, 4}) >= 0, "MP4 preview lost its clean aperture or showed a preroll frame");
    const auto edited_epoch = seen.epoch;
    seen.seek = 2.1F;
    Until(ui, failure, [&] { return seen.epoch > edited_epoch && seen.first_frame && seen.position > 2.12F; },
          "Video did not continue past the trimmed audio after a seek");
    const auto tail_epoch = seen.epoch;
    seen.paused = true;
    seen.seek = 3.95F;
    Until(ui, failure, [&] { return seen.epoch > tail_epoch && seen.first_frame; },
          "Seek beyond the video edit did not retain its last presented picture");
    Check(seen.first_frame->Compare({7, 2}) < 0, "Seek displayed a picture past the video edit");
    preview.Cancel();
    ui.Pump();

    constexpr MediaPresentationRange first_picture_edit{{1, 100}, {3, 100}};
    constexpr auto shortened_samples = OriginalSamples - 22;
    const auto first_picture = EditedClip(*mp4, temporary, first_picture_edit, shortened_samples);
    auto short_input = Require(MediaDemuxer::Open(Require(first_picture->OpenRandomAccess())));
    const auto short_audio = std::ranges::find(short_input.Tracks(), TrackKind::Audio, &MediaTrack::kind);
    Check(short_audio != short_input.Tracks().end() && short_audio->presentation_edit &&
          short_audio->presentation_edit->end == MediaTimestamp{OriginalSamples, 48000} &&
          short_input.PresentationRange(short_audio->id) == MediaPresentationRange{{0, 1}, {shortened_samples, 48000}},
          "AAC presentation range extended past the shortened final packet");
    seen = {};
    preview = Play(tasks, first_picture, seen, failure);
    Until(ui, failure, [&] { return seen.first_frame.has_value(); }, "The edit inside the first picture displayed nothing");
    Check(seen.first_frame == first_picture_edit.start && seen.first_duration == MediaTimestamp{1, 50},
          "The first picture was not clipped to the intersection with the edit");
    preview.Cancel();
    ui.Pump();

    const auto run_export = [&](std::shared_ptr<const File> input, std::optional<ContainerFormat> copy, bool reorder = false) {
      std::shared_ptr<const File> output;
      const auto exporting = tasks.Launch([&, input = std::move(input), copy, reorder]() -> Task<void> {
        try {
          if (copy) output = co_await media_preview::CopyClip(input, temporary, copy);
          else output = co_await media_preview::ExportClip(input, temporary, reorder);
        } catch (...) { failure = std::current_exception(); }
      });
      try {
        Until(ui, failure, [&] { return bool(output); }, "MP4 export did not finish");
      } catch (...) {
        exporting.Cancel();
        throw;
      }
      return output;
    };
    const auto output = run_export(mp4, {});
    CheckExport(*mp4, *output, OriginalSamples);
    CheckExport(*edited, *run_export(edited, {}), EditedSamples);
    CheckExport(*first_picture, *run_export(first_picture, {}), shortened_samples);
    const auto reordered = run_export(mp4, {}, true);
    CheckExport(*mp4, *reordered, OriginalSamples, true);
    CheckCopy(*edited, *run_export(edited, ContainerFormat::Mov), ContainerFormat::Mov);
    CheckCopy(*first_picture, *run_export(first_picture, ContainerFormat::Mp4), ContainerFormat::Mp4);
    const auto hlg = Fixture("preview-hevc-hlg.mov");
    const auto hdr = Require(MediaDemuxer::Open(Require(hlg->OpenRandomAccess())));
    const auto& hdr_track = hdr.Tracks().front();
    Check(hdr_track.pixel_aspect_ratio == MediaTrack::PixelAspectRatio{4, 3}, "HLG fixture lost its pixel aspect");
    for (const auto box : {"colr", "dvvC"}) {
      Check(std::ranges::find(hdr_track.video_extensions, box, &MediaTrack::VideoExtension::type) !=
            hdr_track.video_extensions.end(), "HLG fixture lost a color or Dolby Vision extension");
    }
    const auto hdr_copy = run_export(hlg, ContainerFormat::Mp4);
    CheckCopy(*hlg, *hdr_copy, ContainerFormat::Mp4);
    if (argc >= 2) Check(output->CopyTo(File(argv[1]), true), "Cannot retain export for independent validation");
    if (argc >= 3) Check(reordered->CopyTo(File(argv[2]), true), "Cannot retain reordered export");
    if (argc >= 4) Check(hdr_copy->CopyTo(File(argv[3]), true), "Cannot retain HDR packet copy");
    std::cout << "MP4 seek, presentation edits, B-picture export, and MOV metadata copy passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
