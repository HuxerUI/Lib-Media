#pragma once

#include "format.h"

namespace huxerui::media::detail {
struct AudioEncoderEnd {
  std::optional<std::uint64_t> priming_samples, padding_samples;
};
using AudioEncoderBackendOutput = std::variant<AudioEncodeFormat, EncodedPacket, AudioEncoderEnd>;

// Backend calls stay on the session lane. Submit copies PCM before returning Accepted.
// Input starts at sample zero. Platform packet timing remains relative to that origin.
// At most one packet per submitted block plus kAudioEncoderExtraPackets may be produced.
class AudioEncoderBackend {
public:
  virtual ~AudioEncoderBackend() = default;
  virtual CodecResult<void> Open(const AudioEncoderConfig& config) = 0;
  virtual CodecResult<bool> Ready() { return CodecResult<bool>::Success(true); }
  virtual CodecResult<SubmitStatus> Submit(std::span<const std::byte> pcm, std::uint32_t samples, std::int64_t first_sample) = 0;
  virtual CodecResult<std::optional<AudioEncoderBackendOutput>> Poll() = 0;
  virtual CodecResult<SubmitStatus> Finish() = 0;
  virtual CodecResult<void> Close() = 0;
};
using AudioEncoderFactory = std::function<std::unique_ptr<AudioEncoderBackend>()>;
std::unique_ptr<AudioEncoderBackend> MakeAudioEncoderBackend();

struct VideoEncoderEnd {};
using VideoEncoderBackendOutput = std::variant<VideoEncodeFormat, EncodedPacket, VideoEncoderEnd>;

// Calls stay on the session lane. Each accepted CPU picture produces one access unit, in decode order.
// Input PTS uses microseconds relative to the first accepted input. Output PTS and reported DTS use the same
// timeline with their declared timescales. Submit copies pixels before returning Accepted. Platform callbacks
// retain backend state only and never invoke application callbacks.
class VideoEncoderBackend {
public:
  virtual ~VideoEncoderBackend() = default;
  virtual CodecResult<void> Open(const VideoEncoderConfig& config) = 0;
  virtual CodecResult<bool> Ready() { return CodecResult<bool>::Success(true); }
  virtual CodecResult<SubmitStatus> Submit(const VideoFrame& frame) = 0;
  virtual CodecResult<std::optional<VideoEncoderBackendOutput>> Poll() = 0;
  virtual CodecResult<void> RequestKeyFrame() = 0;
  // Requests nonterminal output progress at an admission boundary; never discards or seals input.
  virtual CodecResult<void> FlushPending() { return CodecResult<void>::Success(); }
  virtual CodecResult<SubmitStatus> Finish() = 0;
  virtual CodecResult<void> Close() = 0;
};
using VideoEncoderFactory = std::function<std::unique_ptr<VideoEncoderBackend>()>;
std::unique_ptr<VideoEncoderBackend> MakeVideoEncoderBackend();
} // namespace huxerui::media::detail
