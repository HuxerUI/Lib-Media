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
} // namespace huxerui::media::detail
