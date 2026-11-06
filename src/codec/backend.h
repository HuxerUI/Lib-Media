#pragma once

#include <huxerui/media_codec.h>
#include <deque>
#include <mutex>

namespace huxerui::media::detail {
using DecoderConfig = std::variant<AudioDecoderConfig, VideoDecoderConfig>;
using DecodedFrame = std::variant<AudioFrame, VideoFrame>;

struct AacConfig {
  std::uint32_t sample_rate;
  std::uint32_t channels;
};
struct AvcConfig {
  unsigned length_size;
  std::vector<Bytes> parameters;
};
CodecResult<AacConfig> ParseAac(std::span<const std::byte> bytes);
CodecResult<AvcConfig> ParseAvc(std::span<const std::byte> bytes);
CodecResult<Bytes> AnnexB(const EncodedPacket& packet, unsigned length_size);
CodecResult<void> ValidatePacket(const EncodedPacket& packet, const DecoderConfig& config, bool needs_key);
CodecResult<std::size_t> ValidateConfig(const DecoderConfig& config);
CodecError Error(CodecErrorCode code, std::string message, std::string platform = {});
BufferReference Own(Bytes bytes);

// Backends are confined to the control lane. Native callbacks retain backend-owned state only.
// One access unit produces at most one frame; accepted work is covered by session output credit.
// Poll drains at most one frame. Empty is temporary; EndOfStream follows a successful Finish.
using BackendOutput = std::variant<DecodedFrame, EndOfStream>;
class DecoderBackend {
public:
  virtual ~DecoderBackend() = default;
  virtual CodecResult<void> Open(const DecoderConfig& config) = 0;
  virtual CodecResult<bool> Ready() { return CodecResult<bool>::Success(true); }
  virtual CodecResult<SubmitStatus> Submit(const EncodedPacket& packet) = 0;
  virtual CodecResult<std::optional<BackendOutput>> Poll() = 0;
  // Match input and output PTS in this timescale before restoring the original packet timestamp.
  virtual std::uint32_t PresentationTimescale() const noexcept { return 1000000; }
  virtual CodecResult<SubmitStatus> Finish() = 0;
  // Cleanup is idempotent and waits for native callbacks/copies on the control lane, never the caller.
  virtual CodecResult<void> Close() = 0;
};
std::unique_ptr<DecoderBackend> MakeDecoderBackend();
using BackendFactory = std::function<std::unique_ptr<DecoderBackend>()>;

// Wraps frame planes with a shared owner, preserving storage and output credit through arbitrary slices.
void RetainFrame(DecodedFrame& frame, const std::shared_ptr<const void>& credit);
std::size_t FrameBytes(const DecodedFrame& frame);
} // namespace huxerui::media::detail
