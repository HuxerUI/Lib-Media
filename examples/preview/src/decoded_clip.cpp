#include "decoded_preview.h"

#if defined(MEDIA_PREVIEW_FRAMES)

#include <fixture_resources.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_playback.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

using namespace huxerui;
using namespace huxerui::media;
using namespace std::chrono_literals;

namespace media_preview {
namespace {

template<class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) {
    const auto& error = result.Error();
    throw std::runtime_error(error.platform_code.empty() ? error.message : error.message + " (" + error.platform_code + ")");
  }
  return std::move(result).Value();
}

struct PacketInput {
  std::vector<EncodedPacket> packets;
  std::size_t next = 0;
  bool sealed = false;

  template<class Decoder> void Feed(Decoder& decoder) {
    while (next < packets.size() && Require(decoder.TrySubmit(packets[next])) == SubmitStatus::Accepted) ++next;
    if (!sealed && next == packets.size()) {
      Require(decoder.Finish());
      sealed = true;
    }
  }
};

using Clock = std::chrono::steady_clock;

VideoDecoderConfig FixtureVideoConfig(Bytes initialization_data) {
  VideoDecoderConfig config;
  config.initialization_data = std::move(initialization_data);
  config.width = 64;
  config.height = 48;
  config.limits.input_items = 4;
  config.limits.output_items = 34;
  config.limits.output_bytes = config.limits.output_items * config.width * config.height * 3 / 2;
  return config;
}

PacketInput ReadPackedTrack(const Resources& resources, RawResource payload, RawResource index) {
  auto bytes = std::make_shared<const Bytes>(resources.GetRawResource(payload).ReadBytes());
  const BufferReference data(*bytes, bytes);
  std::istringstream manifest(resources.GetRawResource(index).ReadString());
  PacketInput input;
  std::size_t offset, size;
  std::int64_t pts, dts, duration;
  std::uint32_t scale;
  bool key_frame;
  while (manifest >> offset >> size >> pts >> dts >> duration >> scale >> key_frame) {
    input.packets.push_back({data.Slice(offset, size), {pts, scale}, MediaTimestamp{dts, scale},
                             MediaTimestamp{duration, scale}, key_frame});
  }
  if (input.packets.empty()) throw std::runtime_error("Empty A/V fixture track");
  return input;
}

constexpr AudioFormat sync_audio{48000, AudioSampleFormat::S16, AudioPacking::Interleaved, AudioChannelLayout::Stereo};
constexpr std::int64_t sync_end = 4 * sync_audio.sample_rate;

std::int64_t AudioTick(MediaTimestamp time) {
  const auto ticks = time.Rescale(sync_audio.sample_rate);
  if (!ticks) throw std::runtime_error("Invalid A/V fixture timestamp");
  return ticks->ticks;
}

void FeedPlayback(DecodedPlayback& playback, const PlaybackEpoch& epoch, AudioDecoder& decoder,
                  std::optional<AudioFrame>& pending, bool& eos) {
  while (!eos) {
    if (pending) {
      if (Require(playback.TrySubmitAudio(epoch, *pending)) == SubmitStatus::WouldBlock) return;
      pending.reset();
    }
    auto output = Require(decoder.TryRead());
    if (!output) return;
    if (std::holds_alternative<EndOfStream>(*output)) {
      Require(playback.FinishAudio(epoch));
      eos = true;
    } else if (auto* frame = std::get_if<AudioFrame>(&*output)) {
      // The fixture has one complete AAC priming frame before zero and a padded final frame.
      const auto pts = AudioTick(frame->pts);
      if (pts + frame->sample_count <= 0 || pts >= sync_end) continue;
      if (pts < 0) throw std::runtime_error("Unexpected partial AAC priming frame");
      const auto count = std::min<std::int64_t>(frame->sample_count, sync_end - pts);
      frame->sample_count = std::uint32_t(count);
      frame->planes.front() = frame->planes.front().Slice(0, std::size_t(count) * sync_audio.ChannelCount() * 2);
      pending = std::move(*frame);
    }
  }
}
} // namespace

Task<std::string> PlayDecodedClip(std::shared_ptr<Resources> resources, std::shared_ptr<ExternalTexture> texture,
                                  bool with_audio, std::function<bool()> paused) {
  auto video_input = ReadPackedTrack(*resources, fixture::raw::sync_avc_bin, fixture::raw::sync_avc_txt);
  const auto video_config = FixtureVideoConfig(resources->GetRawResource(fixture::raw::sync_avc_config).ReadBytes());
  auto video = Require(VideoDecoder::Create(video_config));
  std::optional<AudioDecoder> audio;
  std::optional<PacketInput> audio_input;
  std::optional<AudioPlaybackConfig> playback_config;
  if (with_audio) {
    audio_input = ReadPackedTrack(*resources, fixture::raw::sync_aac_bin, fixture::raw::sync_aac_txt);
    AudioDecoderConfig config;
    config.initialization_data = resources->GetRawResource(fixture::raw::sync_aac_config).ReadBytes();
    config.output = sync_audio;
    config.limits.input_items = 4;
    config.limits.output_items = 4;
    config.limits.output_bytes = 4 * 1024 * 4;
    audio = Require(AudioDecoder::Create(config));
    playback_config.emplace();
    playback_config->format = sync_audio;
#if defined(MEDIA_PREVIEW_IOS)
    playback_config->cleanup_completed = [audio_use = RetainPreviewAudioSession(true)](PlaybackResult<void>) mutable {
      audio_use.reset();
    };
#endif
  }
  auto playback = Require(DecodedPlayback::Create({.audio = std::move(playback_config), .video = true}));
  const auto epoch = playback.Epoch();
  Require(playback.Play());

  std::optional<AudioFrame> pending_audio;
  std::optional<VideoFrame> pending_video;
  bool audio_eos = false, video_eos = false, is_paused = false;
  std::size_t presented = 0, dropped = 0;
  double max_lag_ms = 0;
  auto deadline = Clock::now() + 8s;
  bool matrix_fallback = false, range_fallback = false;
  const std::function<void(const VideoFrame&)> publish = [&](const VideoFrame& frame) {
    matrix_fallback |= frame.format.matrix == ColorMatrix::Unknown;
    range_fallback |= frame.format.range == ColorRange::Unknown;
    PublishFrame(*texture, frame);
  };
  for (;;) {
    if (audio) {
      audio_input->Feed(*audio);
      FeedPlayback(playback, epoch, *audio, pending_audio, audio_eos);
    }
    video_input.Feed(video);
    while (!video_eos) {
      if (pending_video) {
        if (Require(playback.TrySubmitVideo(epoch, *pending_video)) == SubmitStatus::WouldBlock) break;
        pending_video.reset();
      }
      auto output = Require(video.TryRead());
      if (!output) break;
      if (std::holds_alternative<EndOfStream>(*output)) {
        Require(playback.FinishVideo(epoch));
        video_eos = true;
      } else if (auto* frame = std::get_if<VideoFrame>(&*output)) {
        pending_video = std::move(*frame);
      }
    }
    if (paused() != is_paused) {
      is_paused = !is_paused;
      Require(is_paused ? playback.Pause() : playback.Play());
    }
    const auto update = Require(playback.PresentVideo(epoch, publish));
    dropped += update.dropped;
    if (update.lag_ms) {
      max_lag_ms = std::max(max_lag_ms, *update.lag_ms);
      ++presented;
    }
    const auto snapshot = playback.Snapshot();
    if (snapshot.error) throw std::runtime_error(snapshot.error->message);
    if (snapshot.state == PlaybackState::Ended) break;
    if (is_paused) deadline = Clock::now() + 8s;
    if (Clock::now() >= deadline) throw std::runtime_error("Timed out waiting for decoded playback");
    co_await Delay(4ms);
  }
  std::string result = "Ended. Video: " + std::to_string(presented) + " published, " + std::to_string(dropped) +
      " dropped. Max video lag vs playback clock: " + std::to_string(std::int64_t(max_lag_ms)) + " ms.";
  if (matrix_fallback) result += " Unknown matrix uses BT.601.";
  if (range_fallback) result += " Unknown range uses limited range.";
  co_return result;
}
} // namespace media_preview
#else
huxerui::Task<std::string> media_preview::PlayDecodedClip(std::shared_ptr<huxerui::Resources>,
                                                         std::shared_ptr<huxerui::ExternalTexture>,
                                                         bool, std::function<bool()>) {
  co_return "Decoded frame playback is available on Android, iOS, macOS, and Web.";
}
#endif
