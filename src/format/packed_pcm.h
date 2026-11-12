#pragma once

#include <algorithm>
#include <cmath>
#include <huxerui/media_format.h>

namespace huxerui::media::detail {

struct PackedPcmFormat {
  unsigned bits = 0;
  bool big_endian = false;
  bool floating_point = false;
};

constexpr PackedPcmFormat DescribePackedPcm(CodecId codec) {
  switch (codec) {
    case CodecId::PcmU8: return {8};
    case CodecId::PcmS16Le: return {16};
    case CodecId::PcmS16Be: return {16, true};
    case CodecId::PcmS24Le: return {24};
    case CodecId::PcmS24Be: return {24, true};
    case CodecId::PcmS32Le: return {32};
    case CodecId::PcmS32Be: return {32, true};
    case CodecId::PcmF32Le: return {32, false, true};
    case CodecId::PcmF32Be: return {32, true, true};
    default: return {};
  }
}

inline std::int16_t QuantizePcm16(float sample) {
  const auto scaled = std::isfinite(sample) ? std::round(sample * 32768.0f) : 0.0f;
  return static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}

} // namespace huxerui::media::detail
