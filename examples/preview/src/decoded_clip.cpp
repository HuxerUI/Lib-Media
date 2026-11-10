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

float Seconds(MediaTimestamp time) {
  return static_cast<float>(double(time.ticks) / time.timescale);
}

MediaTimestamp TimeAt(float seconds) {
  return {std::llround(double(seconds) * 1000000), 1000000};
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

  MediaTimestamp FindTrimmedAudioEnd(const ContainerPacket& first) {
    // A sync point can precede the final packet. Read the remaining packets before restoring the start.
    auto last = first;
    Require(source.Seek(track.id, {INT64_MAX, 1}));
    while (auto packet = Read()) last = std::move(*packet);
    const auto start = Require(source.Seek(track.id, *first.pts, SeekDirection::AtOrAfter));
    if (!start || start->time != *first.pts) {
      source = Open(*file);
      Require(source.SelectTracks(std::span(&track.id, 1)));
    }
    return AudioEnd(last, track);
  }

  void Seek(MediaTimestamp target, bool video) {
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
    auto source = Open(*file);
    const auto track = Track(source, TrackKind::Video);
    if (!track || (track->codec != CodecId::H264 && track->codec != CodecId::Hevc) ||
        !track->coded_width || !track->coded_height)
      throw std::runtime_error("Decoded preview requires an H.264 or HEVC video track");
    if (track->codec == CodecId::Hevc && track->sample_entry != "hvc1")
      throw std::runtime_error("The HEVC preview requires an hvc1 sample entry");
    if (const auto time = source.Duration()) duration = std::max(0.0F, Seconds(*time));
    if (const auto soundtrack = Track(source, TrackKind::Audio)) {
      if (soundtrack->codec != CodecId::Aac || !soundtrack->sample_rate ||
          (soundtrack->channel_count != 1 && soundtrack->channel_count != 2))
        throw std::runtime_error("Decoded preview requires mono or stereo AAC-LC audio");
      audio = std::make_shared<TrackInput>(file, Open(*file), *soundtrack);
      auto packet = audio->Read();
      if (packet && packet->pts) {
        const auto scale = *soundtrack->sample_rate;
        const auto first = Ticks(*packet->pts, scale);
        const auto priming = soundtrack->priming_samples.value_or(0);
        if (priming > std::uint64_t(INT64_MAX) || first > INT64_MAX - std::int64_t(priming))
          throw std::runtime_error("Invalid AAC leading trim");
        audio_start = {std::max<std::int64_t>(0, first + std::int64_t(priming)), scale};
        if (soundtrack->padding_samples.value_or(0)) {
          audio_end = audio->FindTrimmedAudioEnd(*packet);
          packet = audio->Read();
        }
      }
      audio->buffered = std::move(packet);
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
          auto [packet, last] = co_await OnFileThread([input] { return input->ReadAudio(); });
          next = std::move(packet);
          if (!end && last && next && next->pts && next->duration) end = AudioEnd(*next, input->track);
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

void ApplyVideoAperture(VideoFrame& frame, const MediaTrack& track) {
  if (!track.visible) return;
  const auto crop = *track.visible;
  const auto decoded = frame.format.visible;
  const auto left = std::max(crop.x, decoded.x), top = std::max(crop.y, decoded.y);
  const auto right = std::min(crop.x + crop.width, decoded.x + decoded.width);
  const auto bottom = std::min(crop.y + crop.height, decoded.y + decoded.height);
  if (left >= right || top >= bottom) throw std::runtime_error("MP4 clean aperture lies outside the decoded picture");
  frame.format.visible = {left, top, right - left, bottom - top};
}

bool TrimAudio(AudioFrame& frame, MediaTimestamp target, std::optional<MediaTimestamp> end) {
  const auto scale = frame.format.sample_rate;
  const auto pts = Ticks(frame.pts, scale);
  const auto first = Ticks(target, scale);
  const auto count = std::int64_t(frame.sample_count);
  if (pts > INT64_MAX - count) throw std::runtime_error("Invalid audio frame duration");
  if (pts + count <= first) return false;
  const auto skip = first > pts ? first - pts : 0;
  auto keep = count - skip;
  if (end) {
    const auto cutoff = Ticks(*end, scale);
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
  const auto [width, height] = DisplayDimensions(visible, video_track.rotation);
  Size size{float(width), float(height)};
  controls.reset_video(size);
  const auto publish = [&](const VideoFrame& frame) {
    const auto [frame_width, frame_height] = DisplayDimensions(frame.format.visible, frame.format.rotation);
    const Size frame_size{float(frame_width), float(frame_height)};
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
            ApplyVideoAperture(*frame, video_track);
            frame->format.rotation = video_track.rotation;
            if (frame->pts.Compare(target) >= 0) {
              if (!presented_first) {
                // Publish the requested picture before the playback clock can drop it as late.
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
          // Baseline with B pictures disabled has the same presentation and decode order.
          queue.packet = ContainerPacket{queue.track->id, packet->data, packet->pts, packet->pts,
                                         packet->duration, packet->key_frame};
          ++queue.packets;
        } else if (std::holds_alternative<EndOfStream>(*output)) {
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
            queue.track = MediaTrack{.id = source->track.id, .kind = TrackKind::Video, .codec = CodecId::H264,
                .framing = PacketFraming::LengthPrefixedNals, .initialization_data = format.initialization_data,
                .timescale = source->track.timescale, .coded_width = format.width, .coded_height = format.height,
                .visible = format.visible, .rotation = source->track.rotation};
          }
        }
        co_return true;
      }
    }
    if (frame) {
      if (!encoder) {
        if constexpr (Audio) {
          AudioEncoderConfig config;
          config.input = frame->format;
          encoder.emplace(Require(AudioEncoder::Create(config)));
        } else {
          VideoEncoderConfig config;
          config.input = frame->format;
          config.profile = H264Profile::Baseline;
          config.max_b_frames = 0;
          encoder.emplace(Require(VideoEncoder::Create(config)));
        }
      }
      if (Require(encoder->TrySubmit(*frame)) == SubmitStatus::WouldBlock) co_return false;
      frame.reset();
      co_return true;
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
            frame = std::move(*next);
        } else {
          ApplyVideoAperture(*next, source->track);
          if (lookahead) {
            const auto scale = source->track.timescale.value_or(90000);
            const auto first = Ticks(lookahead->pts, scale), second = Ticks(next->pts, scale);
            if (second <= first || (first < 0 && second > INT64_MAX + first))
              throw std::runtime_error("Invalid video presentation order");
            lookahead->duration = MediaTimestamp{second - first, scale};
            frame = EncoderInput(std::move(*lookahead));
          }
          lookahead = std::move(*next);
        }
      } else if (std::holds_alternative<EndOfStream>(*output)) {
        if constexpr (!Audio) {
          if (lookahead) {
            if (!lookahead->duration || lookahead->duration->ticks <= 0)
              throw std::runtime_error("The final picture needs a positive duration");
            frame = EncoderInput(std::move(*lookahead));
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
  std::optional<FrameType> frame;
  std::optional<FrameType> lookahead;
  MediaTimestamp audio_start{0, 1};
  bool decoded_end = false;
  bool finish_sent = false;
};

} // namespace

Task<std::shared_ptr<const File>> ExportClip(std::shared_ptr<const File> file, File temporary) {
  auto clip = co_await OnFileThread([file] { return ClipInput(file); });
  if (clip.video->track.codec != CodecId::H264)
    throw std::runtime_error("MP4 encoding in the preview requires an eight-bit H.264 source");
  ExportTrack<VideoDecoder, VideoEncoder, VideoFrame> video(
      clip.video, Require(VideoDecoder::Create(VideoConfig(clip.video->track))));
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
  const auto layout = ContainerLayout::FastStart;
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
      mux = co_await OnFileThread([output, tracks = std::move(tracks), layout] {
        Require(MediaMuxer::ValidateOutput(ContainerFormat::Mp4, tracks, layout));
        return std::make_shared<MediaMuxer>(Require(MediaMuxer::Open(
            Require(output->OpenRandomAccess(FileAccessMode::ReadWrite)), ContainerFormat::Mp4, tracks, layout)));
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
    Require(mux->Finish());
    auto reopened = Open(*output);
    std::size_t packets = 0;
    while (auto item = Require(reopened.Read()))
      if (std::holds_alternative<ContainerPacket>(*item)) ++packets;
    if (packets != expected) throw std::runtime_error("The exported MP4 changed its packet count");
    return true;
  });
  co_return output;
}
} // namespace media_preview
