#include "decoded_preview.h"

#include <fixture_resources.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#include <huxerui/media_playback.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <type_traits>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace media_preview {
namespace {

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

template<class Operation> Task<std::invoke_result_t<Operation>> OnFileThread(Operation operation) {
#ifdef __EMSCRIPTEN__
  co_return operation();
#else
  co_return co_await RunWorker(std::move(operation));
#endif
}

MediaDemuxer Open(const File& file) {
  return Require(MediaDemuxer::Open(Require(file.OpenRandomAccess())));
}

MediaDemuxer OpenIndexed(const File& file) {
  auto source = Open(file);
  if (source.Layout() == ContainerLayout::Fragmented)
    Require(source.FindSeekPoint(source.Tracks().front().id, {0, 1}));
  return source;
}

std::optional<MediaTrack> Track(const MediaDemuxer& source, TrackKind kind) {
  const auto tracks = source.Tracks();
  const auto found = std::ranges::find(tracks, kind, &MediaTrack::kind);
  if (found == tracks.end()) return {};
  return *found;
}

std::int64_t Ticks(MediaTimestamp time, std::uint32_t scale) {
  const auto value = time.Rescale(scale);
  if (!value) throw std::runtime_error("Preview timestamp is outside its time grid");
  return value->ticks;
}

std::uint32_t CommonClock(std::uint32_t first, std::uint32_t second) {
  const auto scale = std::lcm(std::uint64_t(first), std::uint64_t(second));
  if (!scale || scale > UINT32_MAX) throw std::runtime_error("Preview clocks have no exact common scale");
  return static_cast<std::uint32_t>(scale);
}

std::int64_t PositiveDistance(std::int64_t end, std::int64_t start) {
  if (end <= start || (start < 0 && end > INT64_MAX + start))
    throw std::runtime_error("Invalid preview interval");
  return end - start;
}

std::int64_t CeilTicks(MediaTimestamp time, std::uint32_t scale) {
  auto ticks = Ticks(time, scale);
  if (MediaTimestamp{ticks, scale}.Compare(time) < 0) {
    if (ticks == INT64_MAX) throw std::runtime_error("Presentation boundary exceeds its clock");
    ++ticks;
  }
  return ticks;
}

float Seconds(MediaTimestamp time) {
  return static_cast<float>(double(time.ticks) / time.timescale);
}

MediaTimestamp TimeAt(float seconds) {
  return {std::llround(double(seconds) * 1000000), 1000000};
}

bool HasHdrMetadata(const MediaTrack& track) {
  return std::ranges::any_of(track.video_extensions, [](const auto& extension) {
    if (extension.type == "dvcC" || extension.type == "dvvC") return true;
    if (extension.type != "colr" || extension.payload.size() < 10) return false;
    const std::string_view type(reinterpret_cast<const char*>(extension.payload.data()), 4);
    if (type != "nclc" && type != "nclx") return false;
    const auto transfer = (std::to_integer<unsigned>(extension.payload[6]) << 8) |
                          std::to_integer<unsigned>(extension.payload[7]);
    return transfer == 16 || transfer == 18;
  });
}

MediaTimestamp AudioEnd(const ContainerPacket& packet, const MediaTrack& track) {
  if (!packet.pts || !packet.duration) throw std::runtime_error("Missing final AAC packet timing");
  const auto scale = *track.sample_rate;
  const auto start = Ticks(*packet.pts, scale);
  const auto length = Ticks(*packet.duration, scale);
  if (length < 0 || start > INT64_MAX - length) throw std::runtime_error("Invalid audio packet duration");
  const auto end = start + length;
  const auto padding = track.padding_samples.value_or(0);
  if (padding > std::uint64_t(INT64_MAX) || end < INT64_MIN + std::int64_t(padding))
    throw std::runtime_error("Invalid AAC trailing trim");
  return {end - std::int64_t(padding), scale};
}

struct TrackInput {
  TrackInput(std::shared_ptr<const File> file, MediaDemuxer source, MediaTrack track)
      : file(std::move(file)), source(std::move(source)), track(std::move(track)) {
    Require(this->source.SelectTracks(std::span(&this->track.id, 1)));
  }

  std::optional<ContainerPacket> Read() {
    if (buffered) return std::exchange(buffered, std::nullopt);
    while (auto item = Require(source.Read())) {
      if (auto* packet = std::get_if<ContainerPacket>(&*item)) return std::move(*packet);
      const auto& update = std::get<MediaTrack>(*item);
      if (update.codec != track.codec || update.initialization_data != track.initialization_data)
        throw std::runtime_error("The preview requires one codec configuration per track");
    }
    return {};
  }

  std::pair<std::optional<ContainerPacket>, bool> ReadAudio() {
    auto packet = Read();
    if (packet) buffered = Read();
    return {std::move(packet), !buffered};
  }

  std::optional<MediaPresentationRange> PresentationRange() const { return source.PresentationRange(track.id); }

  void Seek(MediaTimestamp target, bool video) {
    if (const auto range = video ? PresentationRange() : std::nullopt; range && range->start != range->end) {
      if (target.Compare(range->start) < 0) target = range->start;
      if (target.Compare(range->end) >= 0) {
        const auto scale = track.timescale.value_or(range->end.timescale);
        const auto end = CeilTicks(range->end, scale);
        if (end == INT64_MIN) throw std::runtime_error("No seek position inside the track presentation interval");
        target = {end - 1, scale};
      }
    }
    auto point = Require(source.Seek(track.id, target));
    if (!point) point = Require(source.Seek(track.id, target, SeekDirection::AtOrAfter));
    if (!point || (video && point->kind != SeekPoint::Kind::Independent))
      throw std::runtime_error("The file has no independent seek point at this position");
    buffered.reset();
  }

  std::shared_ptr<const File> file;
  MediaDemuxer source;
  MediaTrack track;
  std::optional<ContainerPacket> buffered;
};

struct ClipInput {
  explicit ClipInput(std::shared_ptr<const File> file) {
    auto source = OpenIndexed(*file);
    const auto track = Track(source, TrackKind::Video);
    if (!track || (track->codec != CodecId::H264 && track->codec != CodecId::Hevc) ||
        !track->coded_width || !track->coded_height)
      throw std::runtime_error("Decoded preview requires an H.264 or HEVC video track");
    if (track->codec == CodecId::Hevc && track->sample_entry != "hvc1")
      throw std::runtime_error("The HEVC preview requires an hvc1 sample entry");
    if (HasHdrMetadata(*track)) throw std::runtime_error("HDR display needs an HDR surface. Use Copy Movie to retain the source metadata.");
    if (const auto time = source.Duration()) duration = std::max(0.0F, Seconds(*time));
    if (const auto soundtrack = Track(source, TrackKind::Audio)) {
      if (soundtrack->codec != CodecId::Aac || !soundtrack->sample_rate ||
          (soundtrack->channel_count != 1 && soundtrack->channel_count != 2))
        throw std::runtime_error("Decoded preview requires mono or stereo AAC-LC audio");
      audio = std::make_shared<TrackInput>(file, Open(*file), *soundtrack);
      if (const auto range = source.PresentationRange(soundtrack->id)) {
        if (range->start.ticks > 0) audio_start = range->start;
        audio_end = range->end;
      } else {
        audio->buffered = audio->Read();
        if (audio->buffered && audio->buffered->pts) {
          const auto scale = *soundtrack->sample_rate;
          const auto first = Ticks(*audio->buffered->pts, scale);
          const auto priming = soundtrack->priming_samples.value_or(0);
          if (priming > std::uint64_t(INT64_MAX) || first > INT64_MAX - std::int64_t(priming))
            throw std::runtime_error("Invalid AAC leading trim");
          audio_start = {std::max<std::int64_t>(0, first + std::int64_t(priming)), scale};
        }
      }
    }
    video = std::make_shared<TrackInput>(std::move(file), std::move(source), *track);
  }
  std::shared_ptr<TrackInput> video;
  std::shared_ptr<TrackInput> audio;
  MediaTimestamp audio_start{0, 1};
  std::optional<MediaTimestamp> audio_end;
  float duration = 0;
};

struct PacketFeed {
  std::optional<ContainerPacket> next;
  std::optional<MediaTimestamp> end;
  bool sealed = false;

  template<class Decoder> Task<bool> Feed(std::shared_ptr<TrackInput> input, Decoder& decoder) {
    if (sealed) co_return false;
    bool advanced = false;
    for (unsigned i = 0; i < 8; ++i) {
      if (!next) {
        if constexpr (std::is_same_v<Decoder, AudioDecoder>) {
          if (end) {
            next = co_await OnFileThread([input] { return input->Read(); });
          } else {
            auto [packet, last] = co_await OnFileThread([input] { return input->ReadAudio(); });
            next = std::move(packet);
            if (last && next && next->pts && next->duration) end = AudioEnd(*next, input->track);
          }
        } else {
          next = co_await OnFileThread([input] { return input->Read(); });
        }
      }
      if (!next) {
        Require(decoder.Finish());
        sealed = true;
        co_return true;
      }
      if (!next->pts) throw std::runtime_error("The preview requires packet presentation timestamps");
      const EncodedPacket packet{next->data, *next->pts, next->dts, next->duration, next->sync_sample.value_or(false)};
      if (Require(decoder.TrySubmit(packet)) == SubmitStatus::WouldBlock) break;
      next.reset();
      advanced = true;
    }
    co_return advanced;
  }
};

AudioFormat PlaybackFormat(const MediaTrack& track) {
  return {*track.sample_rate, AudioSampleFormat::S16, AudioPacking::Interleaved,
          track.channel_count == 1 ? AudioChannelLayout::Mono : AudioChannelLayout::Stereo};
}

VideoDecoderConfig VideoConfig(const MediaTrack& track) {
  VideoDecoderConfig video_config;
  video_config.codec = track.codec;
  video_config.initialization_data = track.initialization_data;
  video_config.width = *track.coded_width;
  video_config.height = *track.coded_height;
  if (track.codec == CodecId::H264) {
    video_config.width = (video_config.width + 15) & ~15u;
    video_config.height = (video_config.height + 15) & ~15u;
  }
  if (track.codec == CodecId::Hevc) {
    if (video_config.initialization_data.size() >= 23 &&
        (std::to_integer<unsigned>(video_config.initialization_data[17]) & 7) == 2)
      video_config.output = VideoPixelFormat::P010;
  }
  if (video_config.width > 8192 || video_config.height > 8192)
    throw std::runtime_error("The preview supports coded dimensions up to 8192 pixels");
  const auto luma = std::size_t(video_config.width) * video_config.height;
  const auto chroma = std::size_t((video_config.width + 1) / 2) * ((video_config.height + 1) / 2);
  const auto frame_bytes = (luma + 2 * chroma) * (video_config.output == VideoPixelFormat::P010 ? 2 : 1);
  video_config.limits.output_bytes = std::max(video_config.limits.output_bytes, 34 * frame_bytes);
  return video_config;
}

void ApplyVideoGeometry(VideoFrame& frame, const MediaTrack& track) {
  if (const auto& aspect = track.pixel_aspect_ratio) {
    frame.format.aspect_width = aspect->width;
    frame.format.aspect_height = aspect->height;
  }
  if (!track.visible) return;
  const auto crop = *track.visible;
  const auto decoded = frame.format.visible;
  const auto left = std::max(crop.x, decoded.x), top = std::max(crop.y, decoded.y);
  const auto right = std::min(crop.x + crop.width, decoded.x + decoded.width);
  const auto bottom = std::min(crop.y + crop.height, decoded.y + decoded.height);
  if (left >= right || top >= bottom) throw std::runtime_error("MP4 clean aperture lies outside the decoded picture");
  frame.format.visible = {left, top, right - left, bottom - top};
}

Size PresentationSize(VideoRectangle area, VideoRotation rotation, double pixel_aspect) {
  const auto width = static_cast<float>(area.width * pixel_aspect), height = float(area.height);
  if (rotation == VideoRotation::Clockwise90 || rotation == VideoRotation::Clockwise270) return {height, width};
  return {width, height};
}

bool LimitPicture(VideoFrame& frame, const std::optional<MediaPresentationRange>& range) {
  if (!range) return true;
  if (range->start == range->end || frame.pts.Compare(range->end) >= 0) return false;
  const auto before_start = frame.pts.Compare(range->start) < 0;
  if (!frame.duration) return !before_start;
  const auto scale = CommonClock(frame.pts.timescale, frame.duration->timescale);
  const auto ticks = Ticks(frame.pts, scale), duration = Ticks(*frame.duration, scale);
  if (duration <= 0 || ticks > INT64_MAX - duration) throw std::runtime_error("Invalid picture interval");
  const MediaTimestamp end{ticks + duration, scale};
  if (end.Compare(range->start) <= 0) return false;
  if (before_start || end.Compare(range->end) > 0) {
    const auto first = before_start ? range->start : frame.pts;
    const auto last = end.Compare(range->end) > 0 ? range->end : end;
    const auto clock = CommonClock(first.timescale, last.timescale);
    frame.pts = first;
    frame.duration = MediaTimestamp{PositiveDistance(Ticks(last, clock), Ticks(first, clock)), clock};
  }
  return true;
}

bool TrimAudio(AudioFrame& frame, MediaTimestamp target, std::optional<MediaTimestamp> end) {
  const auto scale = frame.format.sample_rate;
  const auto pts = Ticks(frame.pts, scale);
  const auto first = CeilTicks(target, scale);
  const auto count = std::int64_t(frame.sample_count);
  if (pts > INT64_MAX - count) throw std::runtime_error("Invalid audio frame duration");
  if (pts + count <= first) return false;
  const auto skip = first > pts ? first - pts : 0;
  auto keep = count - skip;
  if (end) {
    const auto cutoff = CeilTicks(*end, scale);
    const auto position = pts + skip;
    if (cutoff <= position) return false;
    if (cutoff < pts + count) keep = cutoff - position;
  }
  const auto stride = std::size_t(frame.format.ChannelCount()) * 2;
  frame.pts = {pts + skip, scale};
  frame.sample_count = static_cast<std::uint32_t>(keep);
  frame.planes.front() = frame.planes.front().Slice(std::size_t(skip) * stride, std::size_t(keep) * stride);
  return true;
}

std::shared_ptr<const File> TemporaryFile(const File& directory, std::string_view suffix) {
  static std::atomic_uint64_t serial = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto file = directory.Child("media-preview-" + std::to_string(now) + "-" +
                                    std::to_string(++serial) + std::string(suffix));
  auto stream = Require(file.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew));
  Require(stream.Close());
  return std::shared_ptr<const File>(new File(file), [](const File* file) {
    try {
      [[maybe_unused]] const bool removed = file->Delete();
    } catch (...) {}
    delete file;
  });
}

} // namespace

Task<std::shared_ptr<const File>> StageClip(File temporary, std::optional<FileReference> selected,
                                          std::shared_ptr<Resources> resources) {
  auto file = co_await OnFileThread([temporary] { return TemporaryFile(temporary, ".input"); });
  if (selected) {
#ifdef __EMSCRIPTEN__
    constexpr std::uint64_t max_preview_bytes = 128 * 1024 * 1024;
    const auto size = selected->Size();
    if (size && *size > max_preview_bytes) throw std::runtime_error("The browser preview accepts files up to 128 MiB");
    auto input = Require(co_await selected->OpenReadAsync());
    auto output = Require(co_await file->OpenWriteAsync());
    std::uint64_t copied = 0;
    for (;;) {
      auto bytes = Require(co_await input.ReadAsync(64 * 1024));
      if (bytes.empty()) break;
      if (bytes.size() > max_preview_bytes - copied)
        throw std::runtime_error("The browser preview accepts files up to 128 MiB");
      copied += bytes.size();
      Require(co_await output.WriteAsync(std::move(bytes)));
    }
    Require(co_await output.CloseAsync());
#else
    if (!co_await selected->ImportToAsync(*file, true)) throw std::runtime_error("Cannot stage the selected file");
#endif
  } else {
    const auto bytes = resources->GetRawResource(fixture::raw::preview_avc_aac_mp4).ReadBytes();
    if (!co_await file->WriteBytesAsync(bytes)) throw std::runtime_error("Cannot stage the bundled MP4");
  }
  co_return file;
}

Task<void> PlayContainer(std::shared_ptr<const File> file, ClipControls controls) {
  auto clip = co_await OnFileThread([file] { return ClipInput(file); });
  const auto& video_track = clip.video->track;
  const auto video_range = clip.video->PresentationRange();
  const auto video_config = VideoConfig(video_track);
  auto video = Require(VideoDecoder::Create(video_config));
  std::optional<AudioDecoder> audio;
  std::optional<AudioPlaybackConfig> audio_playback;
  if (clip.audio) {
    AudioDecoderConfig config;
    config.initialization_data = clip.audio->track.initialization_data;
    config.output = PlaybackFormat(clip.audio->track);
    audio.emplace(Require(AudioDecoder::Create(config)));
    audio_playback.emplace();
    audio_playback->format = config.output;
    if constexpr (preview_audio_session_supported) {
      audio_playback->cleanup_completed = [use = RetainPreviewAudioSession(true)](PlaybackResult<void>) mutable {
        use.reset();
      };
    }
  }
  auto playback = Require(DecodedPlayback::Create({.audio = std::move(audio_playback), .video = true}));
  auto epoch = playback.Epoch();
  const auto visible = video_track.visible.value_or(VideoRectangle{0, 0, *video_track.coded_width, *video_track.coded_height});
  const auto aspect = video_track.pixel_aspect_ratio.value_or(MediaTrack::PixelAspectRatio{});
  auto size = PresentationSize(visible, video_track.rotation, double(aspect.width) / aspect.height);
  controls.reset_video(size);
  const auto publish = [&](const VideoFrame& frame) {
    const auto& format = frame.format;
    const auto frame_size = PresentationSize(format.visible, format.rotation, double(format.aspect_width) / format.aspect_height);
    if (size != frame_size) {
      size = frame_size;
      controls.reset_video(size);
    }
    controls.publish(frame);
  };
  auto target = TimeAt(0);
  bool running = true, playing = true;
  Require(playback.Play());
  controls.position(0, clip.duration);
  auto next_update = std::chrono::steady_clock::now();
  auto deadline = next_update + 15s;
  PacketFeed audio_feed{.end = clip.audio_end}, video_feed;
  std::optional<VideoFrame> picture;
  std::optional<VideoFrame> before_target;
  std::optional<AudioFrame> samples;
  bool presented_first = false;
  bool video_ended = false, audio_ended = !audio;
  for (;;) {
    if (auto seek = controls.take_seek()) {
      target = TimeAt(std::clamp(*seek, 0.0F, clip.duration));
      co_await OnFileThread([video = clip.video, audio = clip.audio, target] {
        video->Seek(target, true);
        if (audio) {
          const auto scale = *audio->track.sample_rate;
          audio->Seek({Ticks(target, scale) - 1024, scale}, false);
        }
        return true;
      });
      picture.reset();
      before_target.reset();
      samples.reset();
      audio_feed = {.end = clip.audio_end};
      video_feed = {};
      video_ended = false;
      presented_first = false;
      audio_ended = !audio;
      Require(video.Reset());
      if (audio) Require(audio->Reset());
      epoch = Require(playback.Reset());
      controls.reset_video(size);
      playing = !controls.paused();
      if (playing) Require(playback.Play());
      running = true;
      controls.position(Seconds(target), clip.duration);
    }

    const bool should_play = !controls.paused();
    if (running && should_play != playing) {
      playing = should_play;
      Require(playing ? playback.Play() : playback.Pause());
    }
    bool advanced = false;
    if (running && (playing || !presented_first)) {
      if (audio && !audio_ended) {
        advanced |= co_await audio_feed.Feed(clip.audio, *audio);
        while (!audio_ended) {
          if (samples) {
            if (Require(playback.TrySubmitAudio(epoch, *samples)) == SubmitStatus::WouldBlock) break;
            samples.reset();
            advanced = true;
          }
          auto item = Require(audio->TryRead());
          if (!item) break;
          if (auto* frame = std::get_if<AudioFrame>(&*item)) {
            const auto start = target.Compare(clip.audio_start) < 0 ? clip.audio_start : target;
            if (TrimAudio(*frame, start, audio_feed.end)) samples = std::move(*frame);
          } else if (std::holds_alternative<EndOfStream>(*item)) {
            Require(playback.FinishAudio(epoch));
            audio_ended = true;
          }
        }
      }
      if (!video_ended) {
        advanced |= co_await video_feed.Feed(clip.video, video);
        while (!video_ended) {
          if (picture) {
            if (Require(playback.TrySubmitVideo(epoch, *picture)) == SubmitStatus::WouldBlock) break;
            picture.reset();
            advanced = true;
          }
          auto item = Require(video.TryRead());
          if (!item) break;
          if (auto* frame = std::get_if<VideoFrame>(&*item)) {
            if (!LimitPicture(*frame, video_range)) continue;
            ApplyVideoGeometry(*frame, video_track);
            frame->format.rotation = video_track.rotation;
            if (frame->pts.Compare(target) >= 0) {
              if (!presented_first) {
                // A later track start stays on the playback clock. A seek inside the track can show its first picture immediately.
                if (!playing || !video_range || video_range->start.Compare(target) <= 0)
                  publish(*frame);
                presented_first = true;
                before_target.reset();
              }
              picture = std::move(*frame);
            } else if (!presented_first) {
              before_target = std::move(*frame);
            }
          } else if (std::holds_alternative<EndOfStream>(*item)) {
            if (!presented_first && before_target) publish(*before_target);
            presented_first = true;
            before_target.reset();
            Require(playback.FinishVideo(epoch));
            video_ended = true;
          }
        }
      }
      const auto update = Require(playback.PresentVideo(epoch, publish));
      advanced |= update.lag_ms.has_value();
      const auto snapshot = playback.Snapshot();
      if (snapshot.error) throw std::runtime_error(snapshot.error->message);
      if (snapshot.state == PlaybackState::Ended) running = false;
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_update && snapshot.position) {
        controls.position(std::clamp(Seconds(*snapshot.position), 0.0F, clip.duration), clip.duration);
        next_update = now + 100ms;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (advanced || !playing || !running) deadline = now + 15s;
    if (now >= deadline) throw std::runtime_error("Timed out waiting for media preview");
    co_await Delay(running && playing ? 4ms : 40ms);
  }
}

namespace {

MediaMuxer OpenMovieOutput(const File& output, ContainerFormat format, std::span<const MediaTrack> tracks) {
  const auto layout = format == ContainerFormat::Mp4 ? ContainerLayout::FastStart : ContainerLayout::Default;
  return Require(MediaMuxer::Open(Require(output.OpenRandomAccess(FileAccessMode::ReadWrite)), format, tracks, layout));
}

void FinishMovie(MediaMuxer& mux, const File& output, std::size_t expected) {
  Require(mux.Finish());
  auto reopened = Open(output);
  std::size_t packets = 0;
  while (auto item = Require(reopened.Read())) {
    if (std::holds_alternative<ContainerPacket>(*item)) ++packets;
  }
  if (packets != expected) throw std::runtime_error("The exported movie changed its packet count");
}

struct MovieCopy {
  MovieCopy(std::shared_ptr<const File> file, const File& temporary, std::optional<ContainerFormat> target) {
    auto source = OpenIndexed(*file);
    const auto format = target.value_or(source.Format());
    const auto movie = [](ContainerFormat value) { return value == ContainerFormat::Mp4 || value == ContainerFormat::Mov; };
    if (!movie(source.Format()) || !movie(format) || source.Tracks().size() > 2)
      throw std::runtime_error("The preview copies MP4 and MOV files with up to two tracks");
    const std::vector descriptions(source.Tracks().begin(), source.Tracks().end());
    output = TemporaryFile(temporary, format == ContainerFormat::Mov ? ".mov" : ".mp4");
    mux.emplace(OpenMovieOutput(*output, format, descriptions));
    for (std::size_t i = 0; i < descriptions.size(); ++i) {
      auto reader = i + 1 == descriptions.size() ? std::move(source) : OpenIndexed(*file);
      tracks.push_back({TrackInput(file, std::move(reader), descriptions[i])});
    }
  }

  bool Pump() {
    for (unsigned i = 0; i < 32; ++i) {
      const auto next = std::ranges::find_if(tracks, [&](const auto& track) {
        return !track.ended && (!requested || track.input.track.id == *requested);
      });
      if (next == tracks.end()) {
        if (requested) throw std::runtime_error("The muxer requested a track that has ended");
        FinishMovie(*mux, *output, packets);
        return true;
      }
      if (!next->packet) next->packet = next->input.Read();
      if (!next->packet) {
        Require(mux->EndTrack(next->input.track.id));
        next->ended = true;
        requested.reset();
      } else {
        requested = Require(mux->TryWrite(*next->packet));
        if (!requested) {
          next->packet.reset();
          ++packets;
        }
      }
    }
    return false;
  }

  struct Track {
    TrackInput input;
    std::optional<ContainerPacket> packet;
    bool ended = false;
  };
  std::shared_ptr<const File> output;
  std::optional<MediaMuxer> mux;
  std::vector<Track> tracks;
  std::optional<std::uint32_t> requested;
  std::size_t packets = 0;
};

VideoFrame EncoderInput(VideoFrame frame) {
  const auto visible = frame.format.visible;
  if (frame.format.pixel_format == VideoPixelFormat::P010 || (visible.x | visible.y | visible.width | visible.height) & 1u)
    throw std::runtime_error("H.264 export requires an even, eight-bit 4:2:0 crop");
  for (std::size_t i = 0; i < frame.planes.size(); ++i) {
    auto& plane = frame.planes[i];
    const auto scale = i ? 2u : 1u;
    const auto offset = visible.y / scale * plane.row_stride + visible.x / scale * plane.pixel_stride;
    plane.data = plane.data.Slice(offset, plane.data.AsBytes().size() - offset);
  }
  frame.format.width = visible.width;
  frame.format.height = visible.height;
  frame.format.visible = {0, 0, visible.width, visible.height};
  frame.format.aspect_width = frame.format.aspect_height = 1;
  frame.format.rotation = VideoRotation::None;
  return frame;
}

struct ExportQueue {
  std::optional<MediaTrack> track;
  std::optional<ContainerPacket> packet;
  AudioEncodeSummary trim;
  std::size_t packets = 0;
  bool ended = false;
  bool written = false;
};

struct DecodeOrder {
  std::uint32_t scale;
  std::optional<ContainerPacket> pending;
  std::int64_t first_dts = 0;
  std::int64_t total_duration = 0;

  std::optional<ContainerPacket> Push(ContainerPacket packet) {
    if (!packet.dts || !packet.duration) throw std::runtime_error("B-picture export requires explicit DTS and duration");
    const auto dts = Ticks(*packet.dts, scale);
    const auto duration = Ticks(*packet.duration, scale);
    if (duration <= 0 || total_duration > INT64_MAX - duration)
      throw std::runtime_error("Invalid encoded video duration");
    total_duration += duration;
    auto ready = std::exchange(pending, std::move(packet));
    if (ready) ready->duration = MediaTimestamp{PositiveDistance(dts, Ticks(*ready->dts, scale)), scale};
    else first_dts = dts;
    return ready;
  }

  std::optional<ContainerPacket> Finish() {
    if (!pending) return {};
    const auto dts = Ticks(*pending->dts, scale);
    const auto elapsed = dts == first_dts ? 0 : PositiveDistance(dts, first_dts);
    pending->duration = MediaTimestamp{PositiveDistance(total_duration, elapsed), scale};
    return std::exchange(pending, std::nullopt);
  }
};

template<class Decoder, class Encoder, class FrameType>
struct ExportTrack {
  static constexpr bool Audio = std::is_same_v<Decoder, AudioDecoder>;
  ExportTrack(std::shared_ptr<TrackInput> source, Decoder decoder)
      : source(std::move(source)), decoder(std::move(decoder)) {}

  Task<bool> Pump() {
    if (queue.packet || queue.ended) co_return false;
    if (encoder) {
      if (auto output = Require(encoder->TryRead())) {
        if (auto* packet = std::get_if<EncodedPacket>(&*output)) {
          if (!queue.track) throw std::runtime_error("Codec packet preceded its configuration");
          // With reordering disabled the presentation order is the decode order.
          auto dts = decode_order ? packet->dts : std::optional{packet->pts};
          ContainerPacket sample{queue.track->id, packet->data, packet->pts, dts, packet->duration, packet->key_frame};
          queue.packet = decode_order ? decode_order->Push(std::move(sample)) : std::optional{std::move(sample)};
          ++queue.packets;
        } else if (std::holds_alternative<EndOfStream>(*output)) {
          if (decode_order) queue.packet = decode_order->Finish();
          queue.ended = true;
        } else {
          if constexpr (Audio) {
            if (const auto* format = std::get_if<AudioEncodeFormat>(&*output)) {
              queue.track = MediaTrack{.id = source->track.id, .kind = TrackKind::Audio, .codec = CodecId::Aac,
                  .initialization_data = format->initialization_data, .timescale = format->sample_rate,
                  .sample_rate = format->sample_rate, .channel_count = source->track.channel_count};
            } else {
              queue.trim = std::get<AudioEncodeSummary>(*output);
              if (!queue.trim.priming_samples || !queue.trim.padding_samples)
                throw std::runtime_error("The AAC encoder cannot report the trim needed for MP4 export");
            }
          } else {
            const auto& format = std::get<VideoEncodeFormat>(*output);
            const auto range = source->PresentationRange();
            queue.track = MediaTrack{.id = source->track.id, .kind = TrackKind::Video, .codec = CodecId::H264,
                .framing = PacketFraming::LengthPrefixedNals, .initialization_data = format.initialization_data,
                .timescale = decode_order ? std::optional{decode_order->scale} : source->track.timescale,
                .coded_width = format.width, .coded_height = format.height,
                .visible = format.visible, .rotation = source->track.rotation,
                .presentation_edit = range && range->start.ticks >= 0 ? range : source->track.presentation_edit,
                .pixel_aspect_ratio = pixel_aspect_ratio};
          }
        }
        co_return true;
      }
    }
    const auto batch = decode_order ? 4u : 1u;
    if (!frames.empty() && (submitting || decoded_end || frames.size() >= batch)) {
      if (!encoder) {
        if constexpr (Audio) {
          AudioEncoderConfig config;
          config.input = frames.front().format;
          encoder.emplace(Require(AudioEncoder::Create(config)));
        } else {
          VideoEncoderConfig config;
          config.input = frames.front().format;
          config.profile = decode_order ? H264Profile::High : H264Profile::Baseline;
          config.max_b_frames = decode_order ? std::nullopt : std::optional<std::uint32_t>{0};
          encoder.emplace(Require(VideoEncoder::Create(config)));
        }
      }
      bool advanced = false;
      while (!frames.empty() && Require(encoder->TrySubmit(frames.front())) == SubmitStatus::Accepted) {
        frames.pop_front();
        advanced = true;
      }
      submitting = !frames.empty();
      co_return advanced;
    }
    if (decoded_end) {
      if (finish_sent) co_return false;
      if (!encoder) throw std::runtime_error("Cannot export a track without decoded frames");
      Require(encoder->Finish());
      finish_sent = true;
      co_return true;
    }
    if (auto output = Require(decoder.TryRead())) {
      if (auto* next = std::get_if<FrameType>(&*output)) {
        if constexpr (Audio) {
          if (TrimAudio(*next, audio_start, feed.end))
            frames.push_back(std::move(*next));
        } else {
          ApplyVideoGeometry(*next, source->track);
          if (lookahead) {
            const auto scale = source->track.timescale.value_or(90000);
            const auto first = Ticks(lookahead->pts, scale), second = Ticks(next->pts, scale);
            lookahead->duration = MediaTimestamp{PositiveDistance(second, first), scale};
            frames.push_back(EncoderInput(std::move(*lookahead)));
          } else if (source->track.pixel_aspect_ratio || next->format.aspect_width != next->format.aspect_height) {
            pixel_aspect_ratio = MediaTrack::PixelAspectRatio{next->format.aspect_width, next->format.aspect_height};
          }
          lookahead = std::move(*next);
        }
      } else if (std::holds_alternative<EndOfStream>(*output)) {
        if constexpr (!Audio) {
          if (lookahead) {
            if (!lookahead->duration || lookahead->duration->ticks <= 0)
              throw std::runtime_error("The final picture needs a positive duration");
            frames.push_back(EncoderInput(std::move(*lookahead)));
            lookahead.reset();
          }
        }
        decoded_end = true;
      }
      co_return true;
    }
    co_return co_await feed.Feed(source, decoder);
  }

  std::shared_ptr<TrackInput> source;
  Decoder decoder;
  std::optional<Encoder> encoder;
  PacketFeed feed;
  ExportQueue queue;
  std::optional<DecodeOrder> decode_order;
  std::deque<FrameType> frames;
  std::optional<FrameType> lookahead;
  std::optional<MediaTrack::PixelAspectRatio> pixel_aspect_ratio;
  MediaTimestamp audio_start{0, 1};
  bool decoded_end = false;
  bool finish_sent = false;
  bool submitting = false;
};

} // namespace

Task<std::shared_ptr<const File>> CopyClip(std::shared_ptr<const File> file, File temporary,
                                          std::optional<ContainerFormat> format) {
  auto copy = co_await OnFileThread([file, temporary, format] { return std::make_shared<MovieCopy>(file, temporary, format); });
  while (!co_await OnFileThread([copy] { return copy->Pump(); })) co_await Delay(0ms);
  co_return copy->output;
}

Task<std::shared_ptr<const File>> ExportClip(std::shared_ptr<const File> file, File temporary, bool reorder) {
  auto clip = co_await OnFileThread([file] { return ClipInput(file); });
  if (clip.video->track.codec != CodecId::H264)
    throw std::runtime_error("MP4 encoding in the preview requires an eight-bit H.264 source");
  if (const auto range = clip.video->PresentationRange(); range && range->start == range->end)
    throw std::runtime_error("The video edit contains no pictures to export");
  ExportTrack<VideoDecoder, VideoEncoder, VideoFrame> video(
      clip.video, Require(VideoDecoder::Create(VideoConfig(clip.video->track))));
  if (reorder) video.decode_order.emplace(DecodeOrder{.scale = CommonClock(clip.video->track.timescale.value_or(90000), 1000000)});
  std::optional<ExportTrack<AudioDecoder, AudioEncoder, AudioFrame>> audio;
  ExportQueue audio_copy;
  if (clip.audio) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(_WIN32)
    // Preserve source trim on backends that can omit AAC encoder delay and padding.
    audio_copy.track = clip.audio->track;
#else
    AudioDecoderConfig config;
    config.initialization_data = clip.audio->track.initialization_data;
    config.output = PlaybackFormat(clip.audio->track);
    audio.emplace(clip.audio, Require(AudioDecoder::Create(config)));
    audio->audio_start = clip.audio_start;
    audio->feed.end = clip.audio_end;
#endif
  }
  auto output = co_await OnFileThread([temporary] { return TemporaryFile(temporary, ".mp4"); });
  std::shared_ptr<MediaMuxer> mux;
  std::optional<std::uint32_t> requested;
  auto deadline = std::chrono::steady_clock::now() + 15s;
  auto* soundtrack = audio ? &audio->queue : clip.audio ? &audio_copy : nullptr;
  const std::array queues{&video.queue, soundtrack};
  for (;;) {
    bool advanced = co_await video.Pump();
    if (audio) advanced |= co_await audio->Pump();
    if (audio_copy.track && !audio_copy.packet && !audio_copy.ended) {
      audio_copy.packet = co_await OnFileThread([source = clip.audio] { return source->Read(); });
      if (audio_copy.packet) ++audio_copy.packets;
      else audio_copy.ended = true;
      advanced = true;
    }
    if (!mux && video.queue.track && (!soundtrack || soundtrack->track)) {
      std::vector tracks{*video.queue.track};
      if (soundtrack) tracks.push_back(*soundtrack->track);
      mux = co_await OnFileThread([output, tracks = std::move(tracks)] {
        return std::make_shared<MediaMuxer>(OpenMovieOutput(*output, ContainerFormat::Mp4, tracks));
      });
    }
    if (mux) {
      for (auto* queue : queues) {
        if (!queue || queue->written) continue;
        const auto id = queue->track->id;
        if (requested && id != *requested) continue;
        if (queue->packet) {
          requested = co_await OnFileThread([mux, packet = *queue->packet] { return Require(mux->TryWrite(packet)); });
          if (!requested) {
            queue->packet.reset();
            advanced = true;
          }
        } else if (queue->ended) {
          co_await OnFileThread([mux, id, trim = queue->trim] {
            Require(mux->EndTrack(id, trim.priming_samples, trim.padding_samples));
            return true;
          });
          queue->written = true;
          requested.reset();
          advanced = true;
        }
      }
      if (video.queue.written && (!soundtrack || soundtrack->written)) break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (advanced) deadline = now + 15s;
    else if (now >= deadline) throw std::runtime_error("Timed out waiting for MP4 export");
    co_await Delay(advanced ? 0ms : 2ms);
  }
  const auto expected = video.queue.packets + (soundtrack ? soundtrack->packets : 0);
  co_await OnFileThread([mux, output, expected] {
    FinishMovie(*mux, *output, expected);
    return true;
  });
  co_return output;
}
} // namespace media_preview
