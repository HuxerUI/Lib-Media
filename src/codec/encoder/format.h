#pragma once

#include "../common.h"

namespace huxerui::media::detail {
// Pending output reserves the packet bound, with separate headroom for encoder delay and drain output.
inline constexpr std::size_t kAudioEncoderPacketBytes = 64 * 1024;
inline constexpr std::size_t kAudioEncoderExtraPackets = 8;

CodecResult<void> ValidateAudioEncoderConfig(const AudioEncoderConfig& config);
// Describes the configured LC stream after platform output rate and channel negotiation has succeeded.
AudioEncodeFormat DescribeAac(const AudioEncoderConfig& config);
// Validates platform ASC and removes only the recognized SBR-absent extension from the public LC description.
CodecResult<AudioEncodeFormat> ReadAudioEncodeFormat(std::span<const std::byte> description, const AudioEncoderConfig& config);
CodecResult<void> ValidateAudioEncodeFormat(const AudioEncodeFormat& format, const AudioEncoderConfig& config);
} // namespace huxerui::media::detail
