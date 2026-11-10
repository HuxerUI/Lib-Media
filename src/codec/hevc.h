#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
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

struct HevcSequence {
  std::array<std::byte, 12> profile;
  unsigned width, height, left, top, visible_width, visible_height;
  unsigned chroma_format, luma_bits, chroma_bits, temporal_layers;
  bool temporal_id_nested;
};

unsigned HevcNalType(HevcBytes nal);
HevcConfiguration ParseHevcConfiguration(HevcBytes config, bool out_of_band, bool collect);
bool IsHevcIdr(HevcBytes sample, unsigned length, bool out_of_band, bool single_layer);
HevcSequence ReadHevcSequence(HevcBytes sps);
bool HevcSliceIsB(HevcBytes slice, const std::vector<HevcBytes>& parameters);
std::vector<std::byte> HevcAnnexBConfiguration(HevcBytes config);
std::string HevcCodecString(HevcBytes config);

} // namespace huxerui::media::detail
