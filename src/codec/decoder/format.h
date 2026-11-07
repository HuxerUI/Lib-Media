#pragma once

#include "../common.h"

namespace huxerui::media::detail {
using DecoderConfig = std::variant<AudioDecoderConfig, VideoDecoderConfig>;
using DecodedFrame = std::variant<AudioFrame, VideoFrame>;

struct AvcConfig {
  unsigned length_size;
  std::vector<Bytes> parameters;
};
CodecResult<AvcConfig> ParseAvc(std::span<const std::byte> bytes);
CodecResult<Bytes> AnnexB(const EncodedPacket& packet, unsigned length_size);
CodecResult<void> ValidatePacket(const EncodedPacket& packet, const DecoderConfig& config, bool needs_key);
// Returns the maximum retained frame size after validating the stream and queue limits.
CodecResult<std::size_t> ValidateConfig(const DecoderConfig& config);
} // namespace huxerui::media::detail
