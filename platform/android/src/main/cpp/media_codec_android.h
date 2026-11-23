#pragma once

#include <huxerui/media_codec.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#if defined(__ANDROID__)
struct AMediaCodec;
struct AMediaCodecInfo;
#endif

namespace huxerui::media::detail {

#if defined(__ANDROID__)
const AMediaCodecInfo* AndroidCodecInfo(AMediaCodec* codec);
#endif

struct AndroidAvcLevel {
  std::int32_t value;
  std::uint32_t macroblocks_per_second;
  std::uint32_t macroblocks_per_picture;
  std::uint32_t bitrate_kbps;
};

// Android's AVCLevel constants paired with H.264 Annex A limits in ascending level order.
inline constexpr std::array android_avc_levels{
    AndroidAvcLevel{0x00001,     1485,     99,     64},
    AndroidAvcLevel{0x00002,     1485,     99,    128},
    AndroidAvcLevel{0x00004,     3000,    396,    192},
    AndroidAvcLevel{0x00008,     6000,    396,    384},
    AndroidAvcLevel{0x00010,    11880,    396,    768},
    AndroidAvcLevel{0x00020,    11880,    396,   2000},
    AndroidAvcLevel{0x00040,    19800,    792,   4000},
    AndroidAvcLevel{0x00080,    20250,   1620,   4000},
    AndroidAvcLevel{0x00100,    40500,   1620,  10000},
    AndroidAvcLevel{0x00200,   108000,   3600,  14000},
    AndroidAvcLevel{0x00400,   216000,   5120,  20000},
    AndroidAvcLevel{0x00800,   245760,   8192,  20000},
    AndroidAvcLevel{0x01000,   245760,   8192,  50000},
    AndroidAvcLevel{0x02000,   522240,   8704,  50000},
    AndroidAvcLevel{0x04000,   589824,  22080, 135000},
    AndroidAvcLevel{0x08000,   983040,  36864, 240000},
    AndroidAvcLevel{0x10000,  2073600,  36864, 240000},
    AndroidAvcLevel{0x20000,  4177920, 139264, 240000},
    AndroidAvcLevel{0x40000,  8355840, 139264, 480000},
    AndroidAvcLevel{0x80000, 16711680, 139264, 800000},
};

// Encoder inputs have already been validated. MediaCodec can use fewer B pictures than the bound.
inline constexpr std::span<const AndroidAvcLevel> AndroidAvcLevelCandidates(const VideoEncoderConfig& config) {
  const auto columns = (std::uint64_t(config.input.width) + 15) / 16;
  const auto rows = (std::uint64_t(config.input.height) + 15) / 16;
  const auto picture = columns * rows;
  const auto longest_side = std::max(columns, rows);
  const auto bitrate_scale = config.profile == H264Profile::High ? 1250u : 1000u;
  const auto first = std::ranges::find_if(android_avc_levels, [&](const AndroidAvcLevel& level) {
    return picture <= level.macroblocks_per_picture &&
        longest_side * longest_side <= 8 * level.macroblocks_per_picture &&
        picture * config.frame_rate_numerator <= std::uint64_t(level.macroblocks_per_second) * config.frame_rate_denominator &&
        config.bitrate <= level.bitrate_kbps * bitrate_scale;
  });
  return {first, android_avc_levels.end()};
}

} // namespace huxerui::media::detail
