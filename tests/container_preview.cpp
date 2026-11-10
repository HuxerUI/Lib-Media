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

std::shared_ptr<const File> EditedClip(const File& source, const File& directory) {
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
    if (track.kind == TrackKind::Video) track.visible = EditedAperture;
    if (track.kind == TrackKind::Audio) {
      track.padding_samples = track.padding_samples.value_or(0) + OriginalSamples - EditedSamples;
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

void CheckExport(const File& source, const File& output, std::int64_t audio_samples) {
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
  std::vector<MediaTimestamp> input_pictures, exported_pictures;
  while (auto item = Require(input.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    if (packet.track_id == video->id && packet.pts && packet.pts->ticks >= 0) input_pictures.push_back(*packet.pts);
  }
  std::int64_t coded_audio = 0;
  while (auto item = Require(exported.Read())) {
    const auto& packet = std::get<ContainerPacket>(*item);
    if (packet.track_id == video->id) {
      Check(packet.pts && packet.pts == packet.dts, "Export reorders its Baseline video");
      exported_pictures.push_back(*packet.pts);
    } else {
      coded_audio += packet.duration->Rescale(48000)->ticks;
    }
  }
  std::ranges::sort(input_pictures, [](MediaTimestamp a, MediaTimestamp b) { return a.Compare(b) < 0; });
  Check(!input_pictures.empty() && input_pictures == exported_pictures, "Export changed picture presentation times");
  Check(coded_audio - std::int64_t(*audio->priming_samples + *audio->padding_samples) == audio_samples,
        "Export changed the trimmed audio length");
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
    Check(seen.format->visible == EditedAperture && seen.size == Size{316.0F, 236.0F},
          "MP4 preview lost the container's clean aperture");
    const auto edited_epoch = seen.epoch;
    seen.seek = 2.1F;
    Until(ui, failure, [&] { return seen.epoch > edited_epoch && seen.first_frame && seen.position > 2.12F; },
          "Video did not continue past the trimmed audio after a seek");
    preview.Cancel();
    ui.Pump();

    const auto export_clip = [&](std::shared_ptr<const File> input) {
      std::shared_ptr<const File> output;
      const auto exporting = tasks.Launch([&, input = std::move(input)]() -> Task<void> {
        try {
          output = co_await media_preview::ExportClip(input, temporary);
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
    const auto output = export_clip(mp4);
    CheckExport(*mp4, *output, OriginalSamples);
    CheckExport(*edited, *export_clip(edited), EditedSamples);
    if (argc == 2) Check(output->CopyTo(File(argv[1]), true), "Cannot retain export for independent validation");
    std::cout << "MP4 seek, clean aperture, AAC trim, Main10 MOV, and export readback passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
