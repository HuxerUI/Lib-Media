#include "decoded_preview.h"

#include <huxerui/huxerui.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#include <huxerui/testing/ui_test.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

#if defined(__APPLE__) && TARGET_OS_OSX
void CheckProResReadback(const File& source, const File& export_file);
void CheckAudioReadback(const File& source, const File& output, MediaTimestamp expected, unsigned channels);
#endif

#ifdef __ANDROID__
namespace {
std::optional<std::atomic<int>> pending_codecs;
}

extern "C" AMediaCodec* __real_AMediaCodec_createDecoderByType(const char* mime);
extern "C" AMediaCodec* __wrap_AMediaCodec_createDecoderByType(const char* mime) {
  auto* decoder = __real_AMediaCodec_createDecoderByType(mime);
  if (decoder && pending_codecs) ++*pending_codecs;
  return decoder;
}

extern "C" media_status_t __real_AMediaCodec_delete(AMediaCodec* codec);
extern "C" media_status_t __wrap_AMediaCodec_delete(AMediaCodec* codec) {
  const auto status = __real_AMediaCodec_delete(codec);
  if (pending_codecs) --*pending_codecs;
  return status;
}
#endif

namespace {

constexpr VideoRectangle EditedAperture{2, 2, 316, 236};
constexpr MediaSampleDescription::PixelAspectRatio EditedAspect{4, 3};
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

std::optional<ContainerPacket> ReadPacket(MediaDemuxer& demux);

std::shared_ptr<const File> EditedClip(const File& source, const File& directory,
    MediaPresentationRange video_edit = {{1, 4}, {7, 2}}, std::optional<std::int64_t> audio_packet_end = {}) {
  auto input = Require(MediaDemuxer::Open(Require(source.OpenRandomAccess())));
  Check(input.Tracks().size() == 2, "The edited preview fixture requires two tracks");
  std::array tracks{input.Tracks()[0], input.Tracks()[1]};
  const auto index = [&](std::uint32_t id) {
    return std::size_t(std::ranges::find(tracks, id, &MediaTrack::id) - tracks.begin());
  };
  std::array<std::vector<ContainerPacket>, 2> packets;
  while (auto packet = ReadPacket(input)) packets.at(index(packet->track_id)).push_back(std::move(*packet));
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    auto& track = tracks[i];
    const auto first_edit = std::ranges::find_if(track.edits, [](const auto& edit) { return edit.media_start.has_value(); });
    const auto media_start = first_edit != track.edits.end() ? *first_edit->media_start : MediaTimestamp{0, 1};
    if (track.kind == TrackKind::Video) {
      auto& description = track.descriptions.front();
      description.visible = EditedAperture;
      description.pixel_aspect_ratio = EditedAspect;
      constexpr std::uint32_t clock = 90000;
      const auto start = video_edit.start.Rescale(clock)->ticks, end = video_edit.end.Rescale(clock)->ticks;
      track.edits = {{{start, clock}, {}},
                    {{end - start, clock}, MediaTimestamp{media_start.Rescale(clock)->ticks + start, clock}}};
    }
    if (track.kind == TrackKind::Audio) {
      Check(first_edit != track.edits.end(), "The AAC fixture needs an authored priming edit");
      if (audio_packet_end) {
        auto& last = packets[i].back();
        const auto duration = *audio_packet_end + media_start.Rescale(48000)->ticks - last.pts->Rescale(48000)->ticks;
        Check(duration > 0 && MediaTimestamp{duration, 48000}.Compare(*last.duration) < 0,
              "The tail fixture must shorten the final AAC packet");
        last.duration = MediaTimestamp{duration, 48000};
      } else {
        first_edit->duration = {EditedSamples, 48000};
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
  bool retain_first_publication = false;
};

TaskHandle Play(const TaskScope& tasks, std::shared_ptr<const File> file, Observations& seen, std::exception_ptr& error,
    VideoStorage storage = VideoStorage::Cpu) {
  return tasks.Launch([file = std::move(file), &seen, &error, storage]() -> Task<void> {
    try {
      std::shared_ptr<ExternalTexture> texture;
      co_await media_preview::PlayContainer(file, {
          .reset_video = [&](Size size, const std::shared_ptr<ExternalTexture>& surface) {
            if (storage == VideoStorage::Platform) texture = surface ? surface : media_preview::MakeFrameTexture(size, storage);
            seen.size = size;
            if (!seen.retain_first_publication) seen.first_frame.reset();
            ++seen.epoch;
          },
          .publish = [&](const VideoFrame& frame) {
            if (!seen.first_frame) {
              Check(frame.Validate() && frame.Storage() == storage,
                    "Preview published invalid frame storage");
              if (texture) {
                media_preview::PublishFrame(*texture, frame);
                Check(texture->Revision() == 1, "The GPU preview did not publish its first image");
              } else {
                Check(!media_preview::ConvertFramePixels(frame).empty(), "Preview could not convert the decoded picture");
              }
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
      }, storage);
    } catch (...) { error = std::current_exception(); }
  });
}

template<class Condition>
void Until(testing::UiTest& ui, const std::exception_ptr& failure, Condition condition, const char* error,
    std::chrono::seconds timeout = 15s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    if (failure) std::rethrow_exception(failure);
    ui.Pump(4ms);
#ifdef __EMSCRIPTEN__
    emscripten_sleep(1);
#else
    std::this_thread::sleep_for(2ms);
#endif
  }
  if (failure) std::rethrow_exception(failure);
  Check(condition(), error);
}

std::vector<MediaPresentationSlice> PictureSlices(const MediaTrack& track, const ContainerPacket& packet) {
  const auto clock = packet.pts->timescale;
  const auto end = MediaTimestamp{packet.pts->ticks + packet.duration->Rescale(clock)->ticks, clock};
  std::vector<MediaPresentationSlice> slices;
  Require(MapToPresentation(track.edits, *packet.pts, end, slices));
  return slices;
}

void AppendPictureTimes(const MediaTrack& track, const ContainerPacket& packet, std::vector<MediaTimestamp>& times) {
  for (const auto& slice : PictureSlices(track, packet)) times.push_back(slice.presentation.start);
}

void CheckExport(const File& source, const File& output, std::int64_t audio_samples, bool reorder = false,
    VideoStorage storage = VideoStorage::Cpu) {
  auto input = Require(MediaDemuxer::Open(Require(source.OpenRandomAccess())));
  auto exported = Require(MediaDemuxer::Open(Require(output.OpenRead())));
  Check(exported.Format() == ContainerFormat::Mp4 && exported.Tracks().size() == 2,
        "Export is not a sequentially readable two-track MP4");
  const auto audio = std::ranges::find(exported.Tracks(), TrackKind::Audio, &MediaTrack::kind);
  const auto video = std::ranges::find(exported.Tracks(), TrackKind::Video, &MediaTrack::kind);
  Check(audio != exported.Tracks().end() && video != exported.Tracks().end() &&
        audio->descriptions.front().codec == CodecId::Aac && video->descriptions.front().codec == CodecId::H264,
        "Export lost the AAC or H.264 track");
  Check(exported.PresentationRange(audio->id) == MediaPresentationRange{{0, 1}, {audio_samples, 48000}},
        "Export changed the effective audio presentation range");
  const auto source_video = std::ranges::find(input.Tracks(), TrackKind::Video, &MediaTrack::kind);
  Check(source_video != input.Tracks().end(), "The source has no video");
  const auto& original = source_video->descriptions.front();
  const auto& encoded = video->descriptions.front();
  auto expected = original.visible.value_or(VideoRectangle{0, 0, *original.coded_width, *original.coded_height});
  if (storage == VideoStorage::Platform) {
    expected.width = std::max(2u, (expected.width / 2) & ~1u);
    expected.height = std::max(2u, (expected.height / 2) & ~1u);
  }
  const auto actual = encoded.visible.value_or(VideoRectangle{0, 0, *encoded.coded_width, *encoded.coded_height});
  Check(actual == VideoRectangle{0, 0, expected.width, expected.height}, "Export lost the source's visible area");
  Check(encoded.pixel_aspect_ratio == original.pixel_aspect_ratio, "Export changed the display aspect ratio");
  Check(exported.PresentationRange(video->id) == input.PresentationRange(source_video->id),
        "Export changed the effective video presentation range");
  std::vector<MediaTimestamp> input_pictures, exported_pictures;
  std::size_t input_count = 0, output_count = 0;
  while (auto packet = ReadPacket(input)) {
    if (packet->track_id != source_video->id) continue;
    ++input_count;
    AppendPictureTimes(*source_video, *packet, input_pictures);
  }
  std::int64_t audible_samples = 0;
  std::vector<MediaPresentationSlice> slices;
  std::optional<MediaTimestamp> last_dts, last_pts;
  bool reordered = false;
  while (auto packet = ReadPacket(exported)) {
    if (packet->track_id == video->id) {
      Check(packet->pts && packet->dts && packet->duration, "Export lost video timing");
      if (last_dts) Check(packet->dts->Compare(*last_dts) > 0, "Export DTS moved backwards");
      if (!reorder) Check(packet->pts == packet->dts, "Export reorders its Baseline video");
      if (last_pts) reordered |= packet->pts->Compare(*last_pts) < 0;
      last_dts = packet->dts;
      last_pts = packet->pts;
      ++output_count;
      AppendPictureTimes(*video, *packet, exported_pictures);
    } else {
      const auto pts = packet->pts->Rescale(48000)->ticks;
      Require(MapToPresentation(audio->edits, {pts, 48000},
          {pts + packet->duration->Rescale(48000)->ticks, 48000}, slices));
      for (const auto& slice : slices)
        audible_samples += slice.presentation.end.Rescale(48000)->ticks - slice.presentation.start.Rescale(48000)->ticks;
    }
  }
  const auto earlier = [](MediaTimestamp a, MediaTimestamp b) { return a.Compare(b) < 0; };
  std::ranges::sort(input_pictures, earlier);
  std::ranges::sort(exported_pictures, earlier);
  Check(input_count == output_count && !input_pictures.empty() && input_pictures == exported_pictures,
        "Export changed picture presentation times");
  Check(audible_samples == audio_samples, "Export changed the trimmed audio length");
  if (reorder) std::cout << "VideoToolbox reordered output: " << reordered << '\n';
}

std::optional<ContainerPacket> ReadPacket(MediaDemuxer& demux) {
  while (auto item = Require(demux.Read())) {
    if (auto* packet = std::get_if<ContainerPacket>(&*item)) return std::move(*packet);
  }
  return {};
}

std::shared_ptr<const File> RunMovieExport(const TaskScope& tasks, testing::UiTest& ui, const File& temporary,
    std::shared_ptr<const File> input, std::optional<ContainerFormat> copy, bool reorder = false,
    VideoStorage storage = VideoStorage::Cpu) {
  std::shared_ptr<const File> output;
  std::exception_ptr failure;
  const auto exporting = tasks.Launch([&, input = std::move(input)]() -> Task<void> {
    try {
      if (copy) output = co_await media_preview::CopyClip(input, temporary, copy);
      else output = co_await media_preview::ExportClip(input, temporary, reorder, storage);
    } catch (...) { failure = std::current_exception(); }
  });
  try {
    Until(ui, failure, [&] { return bool(output); },
          copy ? "Movie packet copy did not finish" :
              storage == VideoStorage::Platform ? "GPU movie export did not finish" : "CPU movie export did not finish", 35s);
  } catch (...) {
    exporting.Cancel();
    throw;
  }
  return output;
}

void CheckCopy(const File& source, const File& output, ContainerFormat format) {
  auto open = [](const File& file) { return Require(MediaDemuxer::Open(Require(file.OpenRandomAccess()))); };
  auto original = open(source);
  auto copy = open(output);
  const auto equal_bytes = [](const BufferReference& a, const BufferReference& b) {
    return std::ranges::equal(a.AsBytes(), b.AsBytes());
  };
  const auto equal_attributes = [&](const auto& a, const auto& b) {
    if (!a || !b) return a.has_value() == b.has_value();
    return a->invisible == b->invisible && equal_bytes(a->block_additions, b->block_additions);
  };
  const bool matroska = original.Format() == ContainerFormat::Matroska && format == ContainerFormat::Matroska;
  Check(copy.Format() == format && copy.Tracks().size() == original.Tracks().size(), "Packet copy changed the track set");
  Check(copy.EbmlTimestampScale() == original.EbmlTimestampScale(), "Packet copy changed the EBML Segment tick");
  if (matroska) {
    const auto before = Require(original.ReadMatroskaMetadata());
    const auto after = Require(copy.ReadMatroskaMetadata());
    Check(before.title == after.title && before.segment_uid == after.segment_uid && equal_bytes(before.chapters, after.chapters) &&
          std::ranges::equal(before.tags, after.tags, equal_bytes) && equal_bytes(before.attachments, after.attachments),
          "Packet copy changed Matroska chapters, tags, or attachments");
  }
  for (const auto& track : original.Tracks()) {
    const auto found = std::ranges::find(copy.Tracks(), track.id, &MediaTrack::id);
    Check(found != copy.Tracks().end() && found->rotation == track.rotation &&
          found->edits == track.edits && found->references == track.references &&
          copy.PresentationRange(track.id) == original.PresentationRange(track.id) &&
          found->priming_samples == track.priming_samples && found->padding_samples == track.padding_samples &&
          found->seek_preroll == track.seek_preroll && found->default_duration == track.default_duration,
          "Packet copy changed the track or its presentation metadata");
    if (matroska && track.matroska) {
      Check(found->matroska && found->matroska->uid == track.matroska->uid &&
            found->matroska->subtitle_codec == track.matroska->subtitle_codec && found->matroska->fields == track.matroska->fields,
            "Packet copy changed Matroska TrackUID, language, or subtitle mapping");
    }
    const auto& descriptions = track.descriptions;
    const auto& copied = found->descriptions;
    Check(descriptions.size() == copied.size(), "Packet copy lost a sample description");
    for (std::size_t i = 0; i < descriptions.size(); ++i) {
      const auto& a = descriptions[i];
      const auto& b = copied[i];
      Check(a.codec == b.codec && a.sample_entry == b.sample_entry && a.visible == b.visible &&
            a.pixel_aspect_ratio == b.pixel_aspect_ratio && a.video_extensions == b.video_extensions &&
            a.initialization_data == b.initialization_data && a.coded_width == b.coded_width && a.coded_height == b.coded_height,
            "Packet copy changed a sample description");
    }
    auto before = open(source), after = open(output);
    Require(before.SelectTracks(std::span(&track.id, 1)));
    Require(after.SelectTracks(std::span(&track.id, 1)));
    while (const auto expected = ReadPacket(before)) {
      const auto actual = ReadPacket(after);
      Check(actual && actual->track_id == expected->track_id && actual->pts == expected->pts &&
            actual->dts == expected->dts && actual->duration == expected->duration && actual->discard_padding == expected->discard_padding &&
            actual->sync_sample == expected->sync_sample &&
            actual->sample_description_index == expected->sample_description_index &&
            equal_bytes(actual->data, expected->data) && equal_attributes(actual->matroska, expected->matroska),
            "Packet copy changed access units or timing");
    }
    Check(!ReadPacket(after), "Packet copy added access units");
  }
}

void CheckAudioCopy(const TaskScope& tasks, testing::UiTest& ui, const File& temporary, const char* name) {
  const auto input = Fixture(name);
  auto source = media_preview::Open(*input);
  if (source.Layout() == ContainerLayout::Fragmented) {
    Check(!source.PresentationRange(source.Tracks().front().id), "Fragment fixture already has an index");
    Require(source.FindSeekPoint(source.Tracks().front().id, {INT64_MAX, 1}));
  }
  const auto& original = source.Tracks().front();
  const auto expected_range = source.PresentationRange(original.id);
  Check(expected_range.has_value(), "Audio fixture is missing its presentation range");
  struct CopyResult {
    std::shared_ptr<const File> file;
    bool dispatched_during_copy = false;
  };
  const auto result = std::make_shared<CopyResult>();
  std::exception_ptr failure;
  const auto copying = tasks.Launch([&]() -> Task<void> {
    try {
      tasks.Post([result] { result->dispatched_during_copy = !result->file; });
      result->file = co_await media_preview::CopyAudioClip(input, temporary);
    } catch (...) { failure = std::current_exception(); }
  });
  Until(ui, failure, [&] { return bool(result->file); }, "Audio packet copy did not complete");
#ifdef __EMSCRIPTEN__
  Check(result->dispatched_during_copy, "Audio packet copy occupied the Web event loop until completion");
#endif
  auto copy = media_preview::Open(*result->file);
  const auto tracks = copy.Tracks();
  const auto tags = copy.AudioMetadata();
  Check(copy.Format() == source.Format() && tracks.front().descriptions.front().codec == original.descriptions.front().codec &&
        copy.PresentationRange(tracks.front().id) == expected_range, "Audio packet copy changed the format or presentation range");
  if (const auto source_tags = source.AudioMetadata())
    Check(tags && tags->title == source_tags->title && tags->cover, "Audio packet copy lost cover metadata");
  while (const auto expected = ReadPacket(source)) {
    const auto actual = ReadPacket(copy);
    Check(actual && actual->pts == expected->pts && actual->dts == expected->dts && actual->duration == expected->duration &&
          std::ranges::equal(actual->data.AsBytes(), expected->data.AsBytes()), "Audio packet copy changed packet bytes or timing");
  }
  Check(!ReadPacket(copy), "Audio packet copy added packets");
  if (copy.Format() == ContainerFormat::Mp4) {
    const auto sequential = MediaDemuxer::Open(Require(result->file->OpenRead()), ContainerFormat::Mp4);
#ifdef __EMSCRIPTEN__
    Check(!sequential.Succeeded() && sequential.Error().code == ContainerErrorCode::NeedRandomAccess,
          "Web M4A output must keep media before the index to avoid relocation");
#else
    Check(sequential.Succeeded(), "Native M4A output lost fast-start layout");
#endif
  }
}

void CheckFragmentedTrim(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  enum class Edits { Trim, InnerGap, OuterGaps };
  for (const auto edits : {Edits::Trim, Edits::InnerGap, Edits::OuterGaps}) {
    auto source = media_preview::Open(*Fixture("fragmented-aac.m4a"));
    auto track = source.Tracks().front();
    const auto scale = *track.descriptions.front().sample_rate;
    constexpr std::int64_t head = 128, tail = 173, original = 22624;
    constexpr std::int64_t leading = 512, trailing = 768;
    std::int64_t samples = original - head - tail;
    if (edits == Edits::InnerGap) {
      track.edits = {{{1024, scale}, MediaTimestamp{head, scale}}, {{256, scale}, {}},
                     {{1024, scale}, MediaTimestamp{8192, scale}}};
      samples = 2304;
    } else if (edits == Edits::OuterGaps) {
      track.edits = {{{leading, scale}, {}}, {{samples, scale}, MediaTimestamp{head, scale}}, {{trailing, scale}, {}}};
      samples += leading + trailing;
    } else {
      track.edits = {{{original - head - tail, scale}, MediaTimestamp{head, scale}}};
    }
    const auto input = media_preview::TemporaryFile(temporary, ".m4a");
    auto mux = Require(MediaMuxer::Open(Require(input->OpenWrite()), ContainerFormat::Mp4, std::span(&track, 1),
                                       {.layout = ContainerLayout::Fragmented}));
    while (auto packet = ReadPacket(source)) Check(!Require(mux.TryWrite(*packet)), "Fragmented AAC write blocked");
    Require(mux.Finish());
    std::shared_ptr<const File> output;
    std::exception_ptr failure;
    const auto exporting = tasks.Launch([&]() -> Task<void> {
      try { output = co_await media_preview::ExportWave(input, temporary); }
      catch (...) { failure = std::current_exception(); }
    });
    Until(ui, failure, [&] { return bool(output); }, "Fragmented AAC could not export a trimmed WAV");
    const auto wave = media_preview::Open(*output);
    Check(wave.Duration() == MediaTimestamp{samples, scale}, "Fragmented AAC WAV has the wrong sample count");
    const auto check_silence = [&](std::int64_t position, std::size_t length) {
      auto seek = media_preview::Open(*output);
      Check(Require(seek.Seek(seek.Tracks().front().id, {position, scale})).has_value(), "Cannot locate the empty audio edit");
      const auto packet = ReadPacket(seek);
      const auto bytes = length * *track.descriptions.front().channel_count * sizeof(std::int16_t);
      Check(packet && packet->data.AsBytes().size() >= bytes &&
            std::ranges::all_of(packet->data.AsBytes().first(bytes), [](std::byte value) {
        return value == std::byte{0};
      }), "The empty audio edit did not produce silence");
    };
    if (edits == Edits::InnerGap) check_silence(1024, 256);
    if (edits == Edits::OuterGaps) {
      check_silence(0, leading);
      check_silence(samples - trailing, trailing);
    }
    exporting.Cancel();
  }
}

void CheckFragmentedAudioStartup(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  auto bytes = Require(Fixture("fragmented-aac.m4a")->ReadBytes());
  bytes.pop_back();
  const auto input = media_preview::TemporaryFile(temporary, ".m4a");
  Check(input->WriteBytes(bytes), "Could not stage a fragmented audio file with an incomplete tail");
  bool started = false;
  std::exception_ptr failure;
  const auto playing = tasks.Launch([&]() -> Task<void> {
    try {
      co_await media_preview::PlayAudio(input, {
          .paused = [] { return true; }, .take_seek = [] { return std::optional<float>{}; },
          .position = [](float, float) {}, .audio_info = [&](bool, const auto&) { started = true; },
      });
    } catch (...) { failure = std::current_exception(); }
  });
  Until(ui, failure, [&] { return started; }, "Audio preview read the entire fragment index before starting");
  playing.Cancel();
  ui.Pump();
}

#if !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IOS)
void CheckDescriptionPreview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  auto large = media_preview::Open(*Fixture("preview-avc-aac.mp4"));
  auto small = media_preview::Open(*Fixture("mkv-avc-aac.mkv"));
  auto read_idr = [](MediaDemuxer& source) {
    const auto track = std::ranges::find(source.Tracks(), TrackKind::Video, &MediaTrack::kind);
    Check(track != source.Tracks().end(), "Missing AVC fixture");
    auto description = *track;
    Require(source.SelectTracks(std::span(&description.id, 1)));
    while (auto packet = ReadPacket(source)) {
      if (packet->sync_sample == true) return std::pair{std::move(description), std::move(*packet)};
    }
    throw std::runtime_error("No IDR in the AVC fixture");
  };
  auto [first, first_packet] = read_idr(large);
  auto [second, second_packet] = read_idr(small);
  first.id = second.id = 1;
  first.timescale = second.timescale = 8;
  first.edits = {{{4, 8}, MediaTimestamp{2, 8}}, {{1, 8}, {}}, {{4, 8}, MediaTimestamp{8, 8}}};
  first.descriptions.push_back(second.descriptions.front());
  const auto clip = media_preview::TemporaryFile(temporary, ".mp4");
  auto mux = Require(MediaMuxer::Open(Require(clip->OpenRandomAccess(FileAccessMode::ReadWrite)),
                                     ContainerFormat::Mp4, std::span(&first, 1)));
  for (int i = 0; i < 16; ++i) {
    auto packet = i < 8 ? first_packet : second_packet;
    packet.track_id = first.id;
    packet.pts = packet.dts = MediaTimestamp{i, 8};
    packet.duration = MediaTimestamp{1, 8};
    packet.sample_description_index = i < 8 ? 1 : 2;
    Check(!Require(mux.TryWrite(packet)), "Description fixture write blocked");
  }
  Require(mux.Finish());

  Observations seen;
  std::exception_ptr failure;
  const auto preview = Play(tasks, clip, seen, failure);
  Until(ui, failure, [&] { return seen.format && seen.format->width == *first.descriptions.front().coded_width; },
        "Preview missed the first AVC configuration");
  Check(seen.first_frame == MediaTimestamp{0, 8}, "Preview did not project the first edit to its presentation start");
  Until(ui, failure, [&] { return seen.format && seen.format->width == *second.descriptions.front().coded_width; },
        "Preview did not drain and switch AVC configuration");
  Check(seen.first_frame && seen.first_frame->Compare({5, 8}) >= 0,
        "Preview presented the new configuration before the second edit");
  seen.paused = true;
  for (const auto [target, width] : {std::pair{0.125F, *first.descriptions.front().coded_width}, {0.75F, *second.descriptions.front().coded_width}}) {
    const auto epoch = seen.epoch;
    seen.seek = target;
    Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.format && seen.format->width == width; },
          "Seek crossed a description or edit boundary with the wrong decoder");
  }
  preview.Cancel();
  ui.Pump();
  const auto copy = RunMovieExport(tasks, ui, temporary, clip, ContainerFormat::Mov);
  CheckCopy(*clip, *copy, ContainerFormat::Mov);

  // An unused trex default can describe HDR even when the actual first sample is SDR.
  first.edits.clear();
  second.edits.clear();
  std::erase_if(first.descriptions.front().video_extensions, [](const auto& extension) { return extension.type == "colr"; });
  first.descriptions.front().video_extensions.push_back({"colr", {std::byte{'n'}, std::byte{'c'}, std::byte{'l'}, std::byte{'x'},
      std::byte{0}, std::byte{9}, std::byte{0}, std::byte{16}, std::byte{0}, std::byte{9}, std::byte{0}}});
  const auto fragmented = media_preview::TemporaryFile(temporary, ".mp4");
  auto fragments = Require(MediaMuxer::Open(Require(fragmented->OpenWrite()), ContainerFormat::Mp4,
                                           std::span(&first, 1), {.layout = ContainerLayout::Fragmented}));
  second_packet.track_id = second.id;
  second_packet.pts = second_packet.dts = MediaTimestamp{0, 8};
  second_packet.duration = MediaTimestamp{1, 8};
  second_packet.sample_description_index = 2;
  Check(!Require(fragments.TryWrite(second_packet)), "Fragmented description fixture write blocked");
  Require(fragments.Finish());
  auto reader = media_preview::Open(*fragmented);
  Check(reader.Tracks().front().descriptions.size() == 2 &&
        std::get<TrackDecodeStart>(*Require(reader.Read())).sample_description_index == 2,
        "The fragmented fixture did not override its default description");
  seen = {};
  const auto sdr_preview = Play(tasks, fragmented, seen, failure);
  Until(ui, failure, [&] { return seen.format && seen.format->width == *second.descriptions.front().coded_width && seen.duration == 0.125F; },
        "Preview did not use the first packet's SDR configuration and discovered duration");
  sdr_preview.Cancel();
  ui.Pump();
}
#endif

template<class Config, class Probe>
bool CodecAvailable(testing::UiTest& ui, const Config& config, Probe probe, const char* name) {
  struct Observation {
    std::optional<CodecResult<void>> result;
    std::atomic<bool> ready = false;
  };
  const auto observation = std::make_shared<Observation>();
  probe(config, [observation](CodecResult<void> result) {
    observation->result.emplace(std::move(result));
    observation->ready = true;
  });
  Until(ui, {}, [&] { return observation->ready.load(); }, "Codec probe timed out");
  const auto& result = *observation->result;
  if (result.Succeeded()) return true;
  const auto& error = result.Error();
  if (error.code != CodecErrorCode::Unsupported)
    throw std::runtime_error(std::string(name) + ": " + error.message + " " + error.platform_code);
  std::cout << name << " unavailable: " << error.message << '\n';
  return false;
}

VideoDecoderConfig VideoFixtureConfig(const MediaSampleDescription& track, VideoPixelFormat output, VideoStorage storage) {
  return {.initialization_data = track.initialization_data, .width = *track.coded_width,
      .height = *track.coded_height, .output = output, .codec = track.codec, .output_storage = storage};
}

void CheckVpxPreview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary,
                     const std::shared_ptr<const File>& input) {
  const auto matroska = RunMovieExport(tasks, ui, temporary, input, ContainerFormat::Matroska);
  CheckCopy(*input, *matroska, ContainerFormat::Matroska);
  const auto webm = RunMovieExport(tasks, ui, temporary, matroska, ContainerFormat::WebM);
  CheckCopy(*input, *webm, ContainerFormat::WebM);
  const auto source = media_preview::Open(*input);
  const auto config = VideoFixtureConfig(source.Tracks().front().descriptions.front(), VideoPixelFormat::NV12, VideoStorage::Cpu);
  const bool vp8 = config.codec == CodecId::Vp8;
  const auto* codec = vp8 ? "VP8" : "VP9 profile 0";
  if (!CodecAvailable(ui, config, VideoDecoder::Probe, codec)) return;
  const AudioDecoderConfig audio{.initialization_data = source.Tracks().back().descriptions.front().initialization_data,
      .output = media_preview::PlaybackFormat(source.Tracks().back().descriptions.front()), .codec = CodecId::Opus};
  if (!CodecAvailable(ui, audio, AudioDecoder::Probe, "Opus decoder")) return;
  Observations seen;
  std::exception_ptr failure;
  const auto playing = Play(tasks, vp8 ? matroska : input, seen, failure);
  Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.15F; },
        "VP8/VP9 and Opus preview did not start");
  for (const float target : {0.93F, 0.18F, 0.78F}) {
    const auto epoch = seen.epoch;
    seen.seek = target;
    Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target + 0.02F; },
          "VP8/VP9 and Opus playback did not resume after seeking");
    Check(media_preview::Seconds(*seen.first_frame) >= target - 0.001F &&
          media_preview::Seconds(*seen.first_frame) < target + 0.1F,
          "VP8/VP9 seek published a picture outside the target frame interval");
  }
  playing.Cancel();
  ui.Pump();
  std::cout << codec << " and Opus passed preview, repeated seek, and WebM/MKV packet-copy readback\n";
}

void CheckMatroskaHevcPreview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  const auto input = Fixture("mkv-hevc-aac.mkv");
  const auto copy = RunMovieExport(tasks, ui, temporary, input, ContainerFormat::Matroska);
  CheckCopy(*input, *copy, ContainerFormat::Matroska);
  const auto source = media_preview::Open(*input);
  const auto config = VideoFixtureConfig(source.Tracks().front().descriptions.front(), VideoPixelFormat::NV12, VideoStorage::Cpu);
  if (!CodecAvailable(ui, config, VideoDecoder::Probe, "Matroska HEVC preview decoder")) return;
  Observations seen;
  std::exception_ptr failure;
  const auto playing = Play(tasks, copy, seen, failure);
  Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.07F; }, "Matroska HEVC preview did not start");
  for (const auto target : {0.25F, 0.08F}) {
    const auto epoch = seen.epoch;
    seen.seek = target;
    Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target + 0.02F; },
          "Matroska HEVC preview did not resume after seeking");
    const auto pts = media_preview::Seconds(*seen.first_frame);
    Check(pts >= target - 0.001F && pts < target + 0.1F, "Matroska HEVC seek displayed the wrong picture");
  }
  playing.Cancel();
  ui.Pump();
  std::cout << "Matroska HEVC passed preview, repeated seek, and packet-copy readback\n";
}

#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
void CheckProResPreview(const TaskScope& tasks, testing::UiTest& ui, const std::shared_ptr<const File>& input,
    std::string_view entry) {
  const auto source = media_preview::Open(*input);
  const auto tracks = source.Tracks();
  const auto& description = tracks.front().descriptions.front();
  Check(source.Format() == ContainerFormat::Mov && description.codec == CodecId::ProRes &&
        description.sample_entry == entry, "ProRes MOV has the wrong sample entry");
  constexpr auto output = VideoPixelFormat::P010;
#if TARGET_OS_IOS
  const auto config = VideoFixtureConfig(description, output, VideoStorage::Cpu);
  if (!CodecAvailable(ui, config, VideoDecoder::Probe, "ProRes 422 preview decoder")) return;
#endif
  Observations seen;
  std::exception_ptr failure;
  const auto playing = Play(tasks, input, seen, failure);
  Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.06F; },
        "ProRes MOV did not publish a picture and advance its PCM clock");
  Check(seen.format->pixel_format == output && seen.size == Size{64, 48} &&
        seen.first_frame == MediaTimestamp{0, 1} && seen.first_duration == MediaTimestamp{1, 8},
        "ProRes preview changed the first picture format or timing");
  for (const auto target : {0.375F, 0.125F, 0.25F}) {
    const auto epoch = seen.epoch;
    seen.seek = target;
    Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target + 0.02F; },
          "ProRes MOV did not resume synchronized playback after seeking");
    Check(seen.first_frame == media_preview::TimeAt(target) && seen.first_duration == MediaTimestamp{1, 8},
          "ProRes MOV seek presented the wrong picture interval");
  }
  playing.Cancel();
  ui.Pump();
  std::cout << "ProRes " << entry << " MOV passed LPCM preview and repeated seek\n";
}
#endif

void CheckAv1Preview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary, const char* name) {
  const auto input = Fixture(name);
  auto source = media_preview::Open(*input);
  const auto format = source.Format();
  const auto& video = source.Tracks().front();
  const auto config = VideoFixtureConfig(video.descriptions.front(), VideoPixelFormat::NV12, VideoStorage::Cpu);
  Check(config.codec == CodecId::Av1, "AV1 source lost its sample mapping");
  if (format == ContainerFormat::Matroska) {
    const auto metadata = Require(source.ReadMatroskaMetadata());
    const auto subtitle = std::ranges::find(source.Tracks(), TrackKind::Subtitle, &MediaTrack::kind);
    Check(metadata.title == "AV1 with captions" && !metadata.chapters.AsBytes().empty() && !metadata.tags.empty() &&
          !metadata.attachments.AsBytes().empty() && subtitle != source.Tracks().end() && subtitle->matroska &&
          subtitle->matroska->subtitle_codec == "S_TEXT/UTF8", "AV1 MKV fixture did not expose its retained metadata and captions");
  }
  const auto copy = RunMovieExport(tasks, ui, temporary, input, format);
  CheckCopy(*input, *copy, format);
  if (format == ContainerFormat::WebM) {
    const auto matroska = RunMovieExport(tasks, ui, temporary, input, ContainerFormat::Matroska);
    CheckCopy(*input, *matroska, ContainerFormat::Matroska);
    const auto webm = RunMovieExport(tasks, ui, temporary, matroska, ContainerFormat::WebM);
    CheckCopy(*input, *webm, ContainerFormat::WebM);
  }
  auto reopened = media_preview::Open(*copy);
  Require(reopened.SelectTracks(std::span(&video.id, 1)));
  for (const auto time : {MediaTimestamp{9, 10}, MediaTimestamp{2, 10}}) {
    const auto point = Require(reopened.Seek(video.id, time));
    const auto packet = ReadPacket(reopened);
    Check(point && point->presentation_time.Compare(time) <= 0 && packet && packet->sync_sample == true &&
          packet->pts == point->decode_start_pts, "AV1 seek did not start at a sync packet");
  }
  if (!CodecAvailable(ui, config, VideoDecoder::Probe, name)) return;
  Observations seen;
  std::exception_ptr failure;
  const auto playing = Play(tasks, copy, seen, failure);
  Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.12F; }, "AV1 preview did not start");
  for (const float target : {0.91F, 0.23F, 0.74F}) {
    const auto epoch = seen.epoch;
    seen.seek = target;
    Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target + 0.02F; },
          "AV1 preview did not resume after seeking");
    const auto pts = media_preview::Seconds(*seen.first_frame);
    Check(pts >= target - 0.001F && pts < target + 0.1F, "AV1 seek displayed the wrong picture");
  }
  playing.Cancel();
  ui.Pump();
  std::cout << name << " passed AV1 preview, repeated seek, and packet-copy readback\n";
}

void CheckHevcMainExport(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
  constexpr auto storage = VideoStorage::Cpu;
#else
  constexpr auto storage = media_preview::gpu_video_supported ? VideoStorage::Platform : VideoStorage::Cpu;
#endif
  auto input = Fixture("preview-hevc-main.mp4");
#if defined(__APPLE__) && TARGET_OS_IOS && TARGET_OS_SIMULATOR
  {
    auto reader = media_preview::Open(*input);
    auto track = reader.Tracks().front();
    constexpr std::uint32_t scale = 90000;
    track.timescale = scale;
    track.edits.clear();
    input = media_preview::TemporaryFile(temporary, ".mp4");
    auto mux = Require(MediaMuxer::Open(Require(input->OpenRandomAccess(FileAccessMode::ReadWrite)),
        ContainerFormat::Mp4, std::span(&track, 1)));
    std::int64_t pts = 0;
    for (unsigned i = 0; i < 19; ++i) {
      auto packet = ReadPacket(reader);
      Check(packet.has_value(), "HEVC fixture has too few packets for variable timing");
      packet->pts = packet->dts = MediaTimestamp{pts, scale};
      packet->duration = MediaTimestamp{i % 2 ? 2103 : 3903, scale};
      pts += packet->duration->ticks;
      Check(!Require(mux.TryWrite(*packet)), "Variable HEVC fixture requested another track");
    }
    Require(mux.Finish());
  }
#endif
  auto source = media_preview::Open(*input);
  const auto& track = source.Tracks().front();
  auto decode = VideoFixtureConfig(track.descriptions.front(), VideoPixelFormat::NV12, storage);
  constexpr bool scale = storage == VideoStorage::Platform && media_preview::gpu_video_scaling_supported;
  const auto width = scale ? (*track.descriptions.front().coded_width / 2) & ~1u : *track.descriptions.front().coded_width;
  const auto height = scale ? (*track.descriptions.front().coded_height / 2) & ~1u : *track.descriptions.front().coded_height;
#ifdef __EMSCRIPTEN__
  decode.output = VideoPixelFormat::Opaque;
#endif
  VideoEncoderConfig encode;
  encode.input = {width, height, {0, 0, width, height}, decode.output};
  encode.input_storage = storage;
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
  encode.max_b_frames = std::nullopt;
#endif
  encode.codec = CodecId::Hevc;
  if (!CodecAvailable(ui, decode, VideoDecoder::Probe, "HEVC Main preview decoder")) return;
  Observations seen;
  std::exception_ptr failure;
  const auto preview = Play(tasks, input, seen, failure, storage);
  Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.15F; }, "HEVC Main preview did not start");
  preview.Cancel();
  ui.Pump();
  if (!CodecAvailable(ui, encode, VideoEncoder::Probe, "HEVC Main preview encoder")) return;
  const auto output = RunMovieExport(tasks, ui, temporary, input, {}, media_preview::preview_reordered_export, storage);
  auto result = media_preview::Open(*output);
  const auto& encoded = result.Tracks().front();
  Check(encoded.descriptions.front().codec == CodecId::Hevc && encoded.descriptions.front().sample_entry == "hvc1", "HEVC export changed its format");
  const auto source_range = source.PresentationRange(track.id), result_range = result.PresentationRange(encoded.id);
  if (source_range && result_range && source_range != result_range) {
    const auto print = [](const MediaPresentationRange& range) {
      std::cerr << range.start.ticks << '/' << range.start.timescale << " to "
                << range.end.ticks << '/' << range.end.timescale << '\n';
    };
    print(*source_range);
    print(*result_range);
  }
  Check(source_range == result_range, "HEVC export changed its presentation range");
  const auto visible = encoded.descriptions.front().visible.value_or(VideoRectangle{0, 0, *encoded.descriptions.front().coded_width, *encoded.descriptions.front().coded_height});
  Check(visible == VideoRectangle{0, 0, width, height}, "HEVC export changed the requested visible dimensions");
  std::vector<MediaPresentationRange> expected_pictures, actual_pictures;
  while (const auto packet = ReadPacket(source))
    for (const auto& slice : PictureSlices(track, *packet)) expected_pictures.push_back(slice.presentation);
  std::optional<MediaTimestamp> next_dts;
  while (const auto packet = ReadPacket(result)) {
    Check(packet->pts && packet->dts && packet->duration &&
          (!next_dts || *packet->dts == *next_dts), "HEVC export lost its contiguous decode timeline");
#ifdef __EMSCRIPTEN__
    Check(packet->pts == packet->dts, "Web HEVC export did not preserve presentation order");
#endif
    next_dts = MediaTimestamp{packet->dts->ticks + packet->duration->Rescale(packet->dts->timescale)->ticks,
                              packet->dts->timescale};
    for (const auto& slice : PictureSlices(encoded, *packet)) actual_pictures.push_back(slice.presentation);
  }
  const auto before = [](const MediaPresentationRange& a, const MediaPresentationRange& b) {
    return a.start.Compare(b.start) < 0;
  };
  std::ranges::sort(expected_pictures, before);
  std::ranges::sort(actual_pictures, before);
  Check(!expected_pictures.empty() && actual_pictures == expected_pictures, "HEVC export changed picture presentation intervals");
  std::cout << "HEVC Main preview export preserved " << actual_pictures.size() << " picture presentation intervals\n";
}

struct AudioPreviewSample {
  std::shared_ptr<const File> file;
  MediaTimestamp duration;
  CodecId wave_codec = CodecId::PcmS16Le;
  bool streaming = false;
  float play_before_seek = 0.03F;
};

std::shared_ptr<const File> CheckAudioPreviewSample(const TaskScope& tasks, testing::UiTest& ui,
    const File& temporary, const AudioPreviewSample& sample) {
  float position = 0;
  float duration = 0;
  std::optional<float> seek;
  std::optional<float> seek_target;
  std::exception_ptr failure;
  const auto playing = tasks.Launch([&]() -> Task<void> {
    try {
      co_await media_preview::PlayAudio(sample.file, {
          .paused = [] { return false; }, .take_seek = [&] { return std::exchange(seek, std::nullopt); },
          .position = [&](float at, float end) {
            position = at;
            duration = end;
            if (seek_target == at) seek_target.reset();
          },
      });
    } catch (...) { failure = std::current_exception(); }
  });
  Until(ui, failure, [&] { return position > sample.play_before_seek; }, "Audio preview did not advance its PCM clock");
  if (sample.streaming) {
    Check(duration < 0.2F, "Streaming WebM scanned to discover duration before preview");
  } else {
    Check(std::abs(duration - media_preview::Seconds(sample.duration)) < 0.01F, "Wrong audio preview duration");
    const auto advance = std::min(0.015F, duration * 0.1F);
    for (float target : {duration * 0.85F, std::min(0.15F, duration * 0.25F), duration * 0.6F}) {
      seek = seek_target = target;
      Until(ui, failure, [&] { return !seek_target && position > target + advance && position < target + 0.2F; },
            "Audio preview did not resume from the requested seek position");
    }
  }
  playing.Cancel();
  ui.Pump();
  std::shared_ptr<const File> wave;
  const auto exporting = tasks.Launch([&]() -> Task<void> {
    try { wave = co_await media_preview::ExportWave(sample.file, temporary); }
    catch (...) { failure = std::current_exception(); }
  });
  Until(ui, failure, [&] { return bool(wave); }, "Audio preview could not export a WAV");
  exporting.Cancel();
  auto output = media_preview::Open(*wave);
  Check(output.Format() == ContainerFormat::Wave && output.Duration() == sample.duration &&
        output.Tracks().front().descriptions.front().codec == sample.wave_codec,
        "Audio WAV readback lost PCM samples or trim");
  return wave;
}

std::shared_ptr<const File> CopyAudioContainer(const File& source, const File& temporary, ContainerFormat target,
    std::string_view suffix, MediaTimestamp expected, unsigned channels) {
  auto reader = media_preview::Open(source);
  auto output = media_preview::TemporaryFile(temporary, suffix);
  auto mux = Require(MediaMuxer::Open(Require(output->OpenRandomAccess(FileAccessMode::ReadWrite)), target, reader.Tracks()));
  while (const auto packet = ReadPacket(reader))
    Check(!Require(mux.TryWrite(*packet)), "Single-track audio copy requested another track");
  Require(mux.Finish());

  auto input = media_preview::Open(source);
  auto copy = media_preview::Open(*output);
  const auto source_tracks = input.Tracks(), output_tracks = copy.Tracks();
  const auto& original = source_tracks.front().descriptions.front();
  const auto& copied = output_tracks.front().descriptions.front();
  const auto track_id = output_tracks.front().id;
  const bool alac = original.codec == CodecId::Alac;
  const MediaPresentationRange audible{{0, expected.timescale}, expected};
  Check(copy.Format() == target && copied.codec == original.codec && copied.initialization_data == original.initialization_data &&
        copied.sample_rate == expected.timescale && copied.channel_count == channels &&
        copy.Duration() == expected && input.PresentationRange(source_tracks.front().id) == audible &&
        copy.PresentationRange(track_id) == audible,
        "Audio copy changed its format or audible interval");
  std::optional<MediaTimestamp> last_pts;
  while (const auto packet = ReadPacket(input)) {
    const auto next = ReadPacket(copy);
    Check(next && packet->pts == next->pts && packet->dts == next->dts && packet->duration == next->duration &&
          std::ranges::equal(packet->data.AsBytes(), next->data.AsBytes()), "Audio copy changed a packet or its coded timing");
    last_pts = packet->pts;
  }
  Check(!ReadPacket(copy), "Audio copy added packets");
  const MediaTimestamp final_sample{expected.ticks - 1, expected.timescale};
  const auto point = Require(copy.Seek(track_id, final_sample, SeekDirection::AtOrBefore));
  Check(point && ReadPacket(copy).has_value() && (alac ?
        point->decode_start_pts == last_pts : point->presentation_time == final_sample),
        "Audio copy could not seek to its final sample or packet");
#if defined(__APPLE__) && TARGET_OS_OSX
  CheckAudioReadback(source, *output, expected, channels);
#endif
  return output;
}

void CheckAudioContainers(const TaskScope& tasks, testing::UiTest& ui, const File& temporary, const File& fixtures) {
  const File aiff = fixtures.Child("preview-pcm.aiff"), caf = fixtures.Child("preview-pcm.caf");
  const auto aiff_copy = CopyAudioContainer(aiff, temporary, ContainerFormat::Aiff, ".aiff", {8821, 44100}, 1);
  const auto caf_copy = CopyAudioContainer(caf, temporary, ContainerFormat::Caf, ".caf", {9600, 48000}, 2);
  const File m4a = fixtures.Child("tagged-alac.m4a"), trimmed = fixtures.Child("trimmed-alac.caf");
  const auto alac_caf = CopyAudioContainer(m4a, temporary, ContainerFormat::Caf, ".caf", {10560, 96000}, 2);
  const auto alac_m4a = CopyAudioContainer(trimmed, temporary, ContainerFormat::Mp4, ".m4a", {10540, 96000}, 2);
  CopyAudioContainer(*alac_caf, temporary, ContainerFormat::Mp4, ".m4a", {10560, 96000}, 2);
  const auto trimmed_caf = CopyAudioContainer(*alac_m4a, temporary, ContainerFormat::Caf, ".caf", {10540, 96000}, 2);
  CopyAudioContainer(fixtures.Child("alac-edit-8821.m4a"), temporary, ContainerFormat::Caf, ".caf", {8821, 44100}, 1);
  CopyAudioContainer(fixtures.Child("alac-edit-8813.m4a"), temporary, ContainerFormat::Caf, ".caf", {8813, 44100}, 1);
  CheckAudioPreviewSample(tasks, ui, temporary, {aiff_copy, {8821, 44100}, CodecId::PcmF32Le});
  CheckAudioPreviewSample(tasks, ui, temporary, {caf_copy, {9600, 48000}, CodecId::PcmF32Le});
#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
  CheckAudioPreviewSample(tasks, ui, temporary, {alac_caf, {10560, 96000}, CodecId::PcmF32Le});
  const auto first = CheckAudioPreviewSample(tasks, ui, temporary, {trimmed_caf, {10540, 96000}, CodecId::PcmF32Le});
  const auto second = CheckAudioPreviewSample(tasks, ui, temporary, {alac_m4a, {10540, 96000}, CodecId::PcmF32Le});
  Check(Require(first->ReadBytes()) == Require(second->ReadBytes()), "CAF and M4A decoded different trimmed ALAC samples");
#endif
  std::cout << "PASS: AIFF/CAF PCM preview and seek, and CAF/M4A ALAC packet copy\n";
}

#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
void CheckAlacPreview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  AudioPreviewSample sample{Fixture("tagged-alac.m4a"), {10560, 96000}, CodecId::PcmF32Le};
  const auto reference = CheckAudioPreviewSample(tasks, ui, temporary, sample);
  auto reader = media_preview::Open(*sample.file);
  sample.file = media_preview::TemporaryFile(temporary, ".m4a");
  auto mux = Require(MediaMuxer::Open(Require(sample.file->OpenRandomAccess(FileAccessMode::ReadWrite)),
      ContainerFormat::Mp4, reader.Tracks()));
  bool prefixed = false;
  while (auto packet = ReadPacket(reader)) {
    if (packet->duration == MediaTimestamp{2368, 96000}) {
      Check(!prefixed, "ALAC fixture has more than one short packet");
      prefixed = true;
      auto bytes = std::make_shared<Bytes>();
      unsigned used = 0;
      const auto append = [&](unsigned value, unsigned count) {
        while (count) {
          if (!used) bytes->push_back(std::byte{});
          bytes->back() |= static_cast<std::byte>(((value >> --count) & 1u) << (7 - used));
          used = (used + 1) % 8;
        }
      };
      // An extended fill element leaves the data stream header unaligned.
      append(6, 3);
      append(15, 4);
      append(1, 8);
      for (unsigned i = 0; i < 15; ++i) append(0, 8);
      // The data stream element aligns its payload after its extended byte count.
      append(4, 3);
      append(0, 4);
      append(1, 1);
      append(255, 8);
      append(1, 8);
      append(0, (8 - used) % 8);
      for (unsigned i = 0; i < 256; ++i) append(0, 8);
      // Another fill element leaves the audio header and partial sample count unaligned.
      append(6, 3);
      append(1, 4);
      append(0, 8);
      for (const auto byte : packet->data.AsBytes()) append(std::to_integer<unsigned>(byte), 8);
      packet->data = BufferReference(*bytes, bytes);
    }
    Check(!Require(mux.TryWrite(*packet)), "ALAC test muxer requested another track");
  }
  Check(prefixed, "ALAC fixture is missing its short final packet");
  Require(mux.Finish());
  const auto wave = CheckAudioPreviewSample(tasks, ui, temporary, sample);
  Check(Require(wave->ReadBytes()) == Require(reference->ReadBytes()), "ALAC auxiliary elements changed decoded PCM");
  std::cout << "ALAC M4A passed preview, repeated seek, and F32 WAV readback with a prefixed short packet\n";
}
#endif

void CheckAudioPreview(const TaskScope& tasks, testing::UiTest& ui, const File& temporary) {
  const auto link = media_preview::TemporaryFile(temporary, ".ogg");
  auto source = media_preview::Open(*Fixture("opus-link.ogg"));
  auto track = source.Tracks().front();
  const auto pre_skip = track.priming_samples.value();
  track.priming_samples = 0;
  track.descriptions.front().initialization_data[10] = track.descriptions.front().initialization_data[11] = std::byte{};
  auto mux = Require(MediaMuxer::Open(Require(link->OpenWrite()), ContainerFormat::Ogg, std::span(&track, 1)));
  while (auto item = ReadPacket(source)) {
    auto packet = std::move(*item);
    packet.pts->ticks += static_cast<std::int64_t>(pre_skip);
    Check(!Require(mux.TryWrite(packet)), "Zero-pre-skip link requested another track");
  }
  Require(mux.Finish());
  const auto chain = media_preview::TemporaryFile(temporary, ".ogg");
  auto links = Require(Fixture("preview-opus.ogg")->ReadBytes());
  const auto next = Require(link->ReadBytes());
  links.insert(links.end(), next.begin(), next.end());
  Check(chain->WriteBytes(links), "Could not stage the chained audio fixture");
  const auto streaming = Fixture("streaming-opus.webm");
  const auto unindexed_av = Fixture("preview-vp9-opus-no-cues.webm");
  const auto mp3 = Fixture("tagged-mp3.mp3");
  auto no_index = Require(MediaDemuxer::Open(Require(streaming->OpenRead())));
  const auto unavailable = no_index.FindSeekPoint(1, {0, 1});
  Check(!no_index.Duration() && !unavailable.Succeeded() && unavailable.Error().code == ContainerErrorCode::IndexUnavailable,
        "Streaming WebM unexpectedly has a duration or seek index");
  const std::array samples{
      AudioPreviewSample{.file = chain, .duration = {107376 + 34344, 48000}, .play_before_seek = 2.28F},
      AudioPreviewSample{Fixture("webm-opus.webm"), {247200, 48000}},
      AudioPreviewSample{.file = streaming, .duration = {35880, 48000}, .streaming = true},
      AudioPreviewSample{.file = unindexed_av, .duration = {72000, 48000}, .streaming = true},
      AudioPreviewSample{Fixture("tagged-flac.flac"), {50142, 44100}},
      AudioPreviewSample{mp3, {50142, 44100}},
      AudioPreviewSample{Fixture("tagged-aac.m4a"), {52800, 48000}},
#if defined(__APPLE__) && TARGET_OS_OSX
      AudioPreviewSample{Fixture("prores-pcm.mov"), {4000, 8000}, CodecId::PcmF32Le},
#endif
  };
  const auto check_signal = [](MediaDemuxer& output, const char* message) {
    const auto packet = ReadPacket(output).value();
    const auto bytes = packet.data.AsBytes();
    constexpr auto sample_bytes = 120 * 2 * sizeof(std::int16_t);
    Check(bytes.size() >= sample_bytes, "WAV packet is too short for the signal check");
    int peak = 0;
    for (std::size_t i = 0; i < sample_bytes; i += sizeof(std::int16_t)) {
      std::int16_t value;
      std::memcpy(&value, bytes.data() + i, sizeof(value));
      peak = std::max(peak, std::abs(int(value)));
    }
    Check(peak > 500, message);
  };
  for (const auto& sample : samples) {
    const auto wave = CheckAudioPreviewSample(tasks, ui, temporary, sample);
    if (sample.file == chain) {
      auto output = media_preview::Open(*wave);
      Check(Require(output.Seek(1, {107376, 48000})).has_value(), "Could not read the second link's first samples");
      Check(std::holds_alternative<TrackDecodeStart>(Require(output.Read()).value()), "WAV seek lost its description");
      check_signal(output, "The first packet of the zero-pre-skip link lost its decoded signal");
    }
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    if (sample.file == mp3) {
      auto output = media_preview::Open(*wave);
      check_signal(output, "MP3 WAV starts with synthesis delay");
    }
#endif
  }
}

} // namespace

#ifdef __EMSCRIPTEN__
int main() {
#else
int main(int argc, char** argv) {
#endif
  try {
    TaskScope tasks;
    const Application app([] { return View{}; }, {
        .application_hooks = {[&](ApplicationContext&) { tasks = UseApplicationTaskScope(); }},
    });
    testing::UiTest ui(app);
    const File temporary(std::filesystem::temp_directory_path().u8string());
#ifdef __EMSCRIPTEN__
    const bool audio_only = EM_ASM_INT({ return new URLSearchParams(location.search).has('audio-containers'); });
    const File fixtures(MEDIA_FIXTURES);
#else
    const bool vpx_only = argc == 3 && std::string_view(argv[1]) == "--vpx";
    const bool audio_only = argc >= 2 && std::string_view(argv[1]) == "--audio-containers";
    const File fixtures(argc == 3 && (vpx_only || audio_only) ? argv[2] : MEDIA_FIXTURES);
#ifdef __ANDROID__
    if (vpx_only) pending_codecs.emplace(0);
#endif
#endif
    if (audio_only) {
      CheckAudioContainers(tasks, ui, temporary, fixtures);
      return 0;
    }
    for (const auto* name : {"webm-vp8-opus.webm", "preview-vp9-opus-no-cues.webm"})
      CheckVpxPreview(tasks, ui, temporary, std::make_shared<const File>(fixtures.Child(name)));
#ifndef __EMSCRIPTEN__
    if (vpx_only) {
#ifdef __ANDROID__
      // Wait for decoder teardown before the headless process exits.
      Until(ui, {}, [] { return pending_codecs->load() == 0; }, "Android preview decoder cleanup did not complete");
#endif
      return 0;
    }
#endif
    CheckMatroskaHevcPreview(tasks, ui, temporary);
    for (const auto* name : {"preview-av1.webm", "preview-av1.mkv", "preview-av1.mp4"})
      CheckAv1Preview(tasks, ui, temporary, name);
    CheckAudioCopy(tasks, ui, temporary, "tagged-mp3.mp3");
    CheckAudioCopy(tasks, ui, temporary, "tagged-aac.m4a");
    CheckAudioCopy(tasks, ui, temporary, "fragmented-aac.m4a");
    CheckFragmentedTrim(tasks, ui, temporary);
    CheckFragmentedAudioStartup(tasks, ui, temporary);
    CheckHevcMainExport(tasks, ui, temporary);
#if defined(__APPLE__) && (TARGET_OS_OSX || TARGET_OS_IOS)
    CheckProResPreview(tasks, ui, Fixture("prores-pcm.mov"), "apcn");
#if TARGET_OS_OSX
    const auto hq = Fixture("prores-hq-pcm.mov");
    CheckProResPreview(tasks, ui, Fixture("prores-proxy-pcm.mov"), "apco");
    CheckProResPreview(tasks, ui, Fixture("prores-lt-pcm.mov"), "apcs");
    CheckProResPreview(tasks, ui, hq, "apch");
    const auto prores_export = RunMovieExport(tasks, ui, temporary, hq, {});
    CheckProResReadback(*hq, *prores_export);
    CheckProResPreview(tasks, ui, prores_export, "apcn");
#endif
    CheckAlacPreview(tasks, ui, temporary);
#endif
#if defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IOS)
    std::cout << "PASS: Preview export and audio packet copy contracts\n";
#else
    CheckDescriptionPreview(tasks, ui, temporary);
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

    if constexpr (media_preview::gpu_video_supported) {
      seen = {};
      preview = Play(tasks, mp4, seen, failure, VideoStorage::Platform);
      Until(ui, failure, [&] { return seen.first_frame && seen.position > 0.15F; }, "GPU preview did not start");
      for (const auto target : {2.1F, 0.6F}) {
        const auto epoch = seen.epoch;
        seen.seek = target;
        Until(ui, failure, [&] { return seen.epoch > epoch && seen.first_frame && seen.position > target; },
              "GPU seek did not publish a new picture");
      }
      preview.Cancel();
      ui.Pump();
      const auto output = RunMovieExport(tasks, ui, temporary, mp4, {}, media_preview::preview_reordered_export, VideoStorage::Platform);
      CheckExport(*mp4, *output, OriginalSamples, media_preview::preview_reordered_export, VideoStorage::Platform);
    }

    const auto main10 = Fixture("preview-hevc10.mov");
    const auto main10_source = media_preview::Open(*main10);
    const auto main10_decode = VideoFixtureConfig(main10_source.Tracks().front().descriptions.front(), VideoPixelFormat::P010, VideoStorage::Cpu);
    const auto main10_available = CodecAvailable(ui, main10_decode, VideoDecoder::Probe, "HEVC Main10 preview decoder");
    if (main10_available) {
      seen = {};
      preview = Play(tasks, main10, seen, failure);
      Until(ui, failure, [&] { return seen.first_frame.has_value(); }, "The Main10 MOV did not publish a picture");
      Check(seen.format->pixel_format == VideoPixelFormat::P010 && seen.format->matrix == ColorMatrix::BT709 &&
            seen.format->rotation == VideoRotation::Clockwise90 && seen.size.width == 96 && seen.size.height == 128,
            "Main10 preview lost bit depth, color, or rotation");
      preview.Cancel();
      ui.Pump();
    }

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
    const auto short_video = std::ranges::find(short_input.Tracks(), TrackKind::Video, &MediaTrack::kind);
    const auto clipped_sync = Require(short_input.FindSeekPoint(short_video->id, {0, 1}, SeekDirection::AtOrAfter));
    Check(clipped_sync && clipped_sync->presentation_time == first_picture_edit.start &&
          clipped_sync->decode_start_pts.Compare(*short_video->edits.back().media_start) < 0,
          "Forward seek lost the decode start of an edit inside the first picture");
    const auto short_audio = std::ranges::find(short_input.Tracks(), TrackKind::Audio, &MediaTrack::kind);
    Check(short_audio != short_input.Tracks().end() && !short_audio->edits.empty() &&
          short_audio->edits.back().duration == MediaTimestamp{OriginalSamples, 48000} &&
          short_input.PresentationRange(short_audio->id) == MediaPresentationRange{{0, 1}, {shortened_samples, 48000}},
          "AAC presentation range extended past the shortened final packet");
    // The audio clock can pass the short edit before the test observes the cleared preview.
    seen = {.retain_first_publication = true};
    preview = Play(tasks, first_picture, seen, failure);
    Until(ui, failure, [&] { return seen.first_frame.has_value(); }, "The edit inside the first picture displayed nothing");
    Check(seen.first_frame == first_picture_edit.start && seen.first_duration == MediaTimestamp{1, 50},
          "The first picture was not clipped to the intersection with the edit");
    preview.Cancel();
    ui.Pump();

    const auto run_export = [&](std::shared_ptr<const File> input, std::optional<ContainerFormat> copy, bool reorder = false) {
      return RunMovieExport(tasks, ui, temporary, std::move(input), copy, reorder);
    };
    const auto output = run_export(mp4, {});
    CheckExport(*mp4, *output, OriginalSamples);
    CheckExport(*edited, *run_export(edited, {}), EditedSamples);
    CheckExport(*first_picture, *run_export(first_picture, {}), shortened_samples);
    std::shared_ptr<const File> reordered;
    if constexpr (media_preview::preview_reordered_export) {
      reordered = run_export(mp4, {}, true);
      CheckExport(*mp4, *reordered, OriginalSamples, true);
    }
    CheckCopy(*edited, *run_export(edited, ContainerFormat::Mov), ContainerFormat::Mov);
    CheckCopy(*first_picture, *run_export(first_picture, ContainerFormat::Mp4), ContainerFormat::Mp4);
    const auto hlg = Fixture("preview-hevc-hlg.mov");
    const auto hdr = Require(MediaDemuxer::Open(Require(hlg->OpenRandomAccess())));
    const auto& hdr_track = hdr.Tracks().front();
    Check(hdr_track.descriptions.front().pixel_aspect_ratio == MediaSampleDescription::PixelAspectRatio{4, 3}, "HLG fixture lost its pixel aspect");
    for (const auto box : {"colr", "dvvC"}) {
      Check(std::ranges::find(hdr_track.descriptions.front().video_extensions, box, &MediaSampleDescription::VideoExtension::type) !=
            hdr_track.descriptions.front().video_extensions.end(), "HLG fixture lost a color or Dolby Vision extension");
    }
    const auto hdr_copy = run_export(hlg, ContainerFormat::Mp4);
    CheckCopy(*hlg, *hdr_copy, ContainerFormat::Mp4);
    const auto width = main10_decode.width, height = main10_decode.height;
    VideoEncoderConfig main10_encode;
    main10_encode.input = {width, height, {0, 0, width, height}, VideoPixelFormat::P010};
    main10_encode.codec = CodecId::Hevc;
    if constexpr (media_preview::preview_reordered_export) main10_encode.max_b_frames = std::nullopt;
    if (main10_available && CodecAvailable(ui, main10_encode, VideoEncoder::Probe, "HEVC Main10 preview encoder")) {
      const auto main10_export = run_export(main10, {}, media_preview::preview_reordered_export);
      const auto hevc = media_preview::Open(*main10_export);
      const auto& encoded_track = hevc.Tracks().front();
      Check(encoded_track.descriptions.front().codec == CodecId::Hevc && encoded_track.descriptions.front().sample_entry == "hvc1" &&
            encoded_track.rotation == VideoRotation::Clockwise90 &&
            encoded_track.descriptions.front().initialization_data.size() > 17 &&
            (std::to_integer<unsigned>(encoded_track.descriptions.front().initialization_data[17]) & 7) == 2,
            "Main10 export lost its codec, rotation, or bit depth");
    }
    CheckAudioPreview(tasks, ui, temporary);
    if (argc >= 2) Check(output->CopyTo(File(argv[1]), true), "Cannot retain export for independent validation");
    if (argc >= 3 && reordered) Check(reordered->CopyTo(File(argv[2]), true), "Cannot retain reordered export");
    if (argc >= 4) Check(hdr_copy->CopyTo(File(argv[3]), true), "Cannot retain HDR packet copy");
    std::cout << "MP4 seek, presentation edits, export, and MOV metadata copy passed\n";
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
