#pragma once

#include "../common.h"

namespace huxerui::media::detail {
using DecoderConfig = std::variant<AudioDecoderConfig, VideoDecoderConfig>;
using DecodedFrame = std::variant<AudioFrame, VideoFrame>;

CodecResult<Bytes> AnnexB(const EncodedPacket& packet, unsigned length_size);
CodecResult<unsigned> AudioSampleLimit(const AudioDecoderConfig& config);
CodecResult<unsigned> AudioPacketSamples(const AudioDecoderConfig& config, std::span<const std::byte> bytes);
// Raw decoder headers contain no container-owned sample delay.
Bytes AudioDecoderHeader(const AudioDecoderConfig& config);
CodecResult<void> ValidatePacket(const EncodedPacket& packet, const DecoderConfig& config, bool needs_key);
// Returns the maximum retained frame size after validating the stream and queue limits.
CodecResult<std::size_t> ValidateConfig(const DecoderConfig& config);
} // namespace huxerui::media::detail
