#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace huxerui::media::detail {

using HevcBytes = std::span<const std::byte>;

struct HevcConfiguration {
  unsigned length;
  unsigned luma_bits;
  unsigned chroma_bits;
  unsigned chroma_format;
  std::vector<HevcBytes> parameters;
};

HevcConfiguration ParseHevcConfiguration(HevcBytes config, bool out_of_band, bool collect);
bool IsHevcIdr(HevcBytes sample, unsigned length, bool out_of_band, bool single_layer);

} // namespace huxerui::media::detail
