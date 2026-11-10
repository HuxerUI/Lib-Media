#pragma once

#include "format.h"

namespace huxerui::media::detail {
// Backends are confined to the control lane. Platform callbacks retain backend-owned state only.
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
  // A surface backend may use the platform's presentation order to keep its buffer leases small.
  virtual bool PresentationOrdered() const noexcept { return false; }
  virtual CodecResult<SubmitStatus> Finish() = 0;
  // Cleanup is idempotent and waits for platform callbacks/copies on the control lane, never the caller.
  virtual CodecResult<void> Close() = 0;
};
std::unique_ptr<DecoderBackend> MakeDecoderBackend();
using BackendFactory = std::function<std::unique_ptr<DecoderBackend>()>;

struct VideoDecoderAccess {
  static CodecResult<VideoDecoder> Create(const VideoDecoderConfig& config, BackendFactory factory);
};
} // namespace huxerui::media::detail
