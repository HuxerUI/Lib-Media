#pragma once

#include "../common.h"

namespace huxerui::media::detail {
CodecResult<VideoEncodeFormat> ReadAnnexBVideoFormat(std::span<const std::byte> parameter_sets, CodecId codec);
// Pending output reserves the packet bound, with separate headroom for encoder delay and drain output.
inline constexpr std::size_t kAudioEncoderPacketBytes = 64 * 1024;
inline constexpr std::size_t kAudioEncoderExtraPackets = 8;

CodecResult<void> ValidateAudioEncoderConfig(const AudioEncoderConfig& config);
// Describes the configured LC stream after platform output rate and channel negotiation has succeeded.
AudioEncodeFormat DescribeAac(const AudioEncoderConfig& config);
// Validates platform ASC and removes only the recognized SBR-absent extension from the public LC description.
CodecResult<AudioEncodeFormat> ReadAudioEncodeFormat(std::span<const std::byte> description, const AudioEncoderConfig& config);
CodecResult<void> ValidateAudioEncodeFormat(const AudioEncodeFormat& format, const AudioEncoderConfig& config);

// Returns the tightly packed input byte count after checking fixed video controls and queue bounds.
CodecResult<std::size_t> ValidateVideoEncoderConfig(const VideoEncoderConfig& config);
// Copies active samples into tightly packed I420, NV12, P010, or P210 storage without changing color values.
void CopyVideoEncoderPixels(const VideoFrame& frame, VideoPixelFormat layout, std::span<std::byte> target);
// Reads a configuration record or Annex B parameter sets for AVC/HEVC. ProRes takes an empty description.
// An encoder that preserves the input YUV values can supply absent AVC color signaling from its configuration.
CodecResult<VideoEncodeFormat> ReadVideoEncodeFormat(std::span<const std::byte> description, const VideoEncoderConfig& config,
    bool complete_avc_color = false);
CodecResult<VideoEncodeFormat> ReadVideoEncodeFormatFromPacket(std::span<const std::byte> packet, const VideoEncoderConfig& config);
// A zero length_size denotes Annex B; 1, 2, or 4 denotes length-prefixed NAL units. Enforces the access-unit bound
// and removes validated AVC base-layer extension prefixes. strip_headers also removes parameter sets and delimiters.
CodecResult<Bytes> NormalizeVideoEncoderPacket(std::span<const std::byte> bytes, unsigned length_size, std::size_t maximum,
    CodecId codec = CodecId::H264, bool strip_headers = false, const VideoEncoderConfig* avc_color_config = nullptr);
// Facts read from one complete picture access unit, independently of platform key-picture flags.
struct VideoEncoderPicture {
  bool key_frame = false;
  bool b_frame = false;
  bool headers = false;
};
// Verifies one picture and any repeated parameter sets against the fixed format without removing NAL units.
CodecResult<VideoEncoderPicture> InspectVideoEncoderPacket(const EncodedPacket& packet, const VideoEncodeFormat& format);
// Chooses a bounded automatic B-picture count for platforms with numeric controls.
std::uint32_t VideoEncoderBFrames(const VideoEncoderConfig& config);
// Returns the full-packet reservation window, capped at 32 for platform progress controls.
std::size_t VideoEncoderWindow(const VideoEncoderConfig& config);
} // namespace huxerui::media::detail
