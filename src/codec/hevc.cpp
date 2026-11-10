#include "hevc.h"

#include <cstdint>
#include <stdexcept>

namespace huxerui::media::detail {
namespace {

unsigned Octet(HevcBytes bytes, std::size_t offset = 0) {
  return std::to_integer<unsigned>(bytes[offset]);
}

unsigned Big16(HevcBytes bytes, std::size_t offset = 0) {
  return (Octet(bytes, offset) << 8) | Octet(bytes, offset + 1);
}

unsigned HevcNalType(HevcBytes nal) {
  if (nal.size() < 2) throw std::invalid_argument("Truncated HEVC NAL header");
  const auto header = Big16(nal);
  if ((header & 0x8000) || !(header & 7)) throw std::invalid_argument("Invalid HEVC NAL header");
  return (header >> 9) & 63;
}

} // namespace

HevcConfiguration ParseHevcConfiguration(HevcBytes config, bool hvc1, bool collect) {
  if (config.size() < 23 || config.size() > 65536 || Octet(config) != 1)
    throw std::invalid_argument("Invalid HEVCDecoderConfigurationRecord");
  const auto length = (Octet(config, 21) & 3u) + 1;
  if (length == 3) throw std::invalid_argument("Invalid HEVC NAL length size");

  HevcConfiguration result{length, 8 + (Octet(config, 17) & 7u), 8 + (Octet(config, 18) & 7u),
                           Octet(config, 16) & 3u, {}};
  auto arrays = config.subspan(23);
  std::uint64_t present = 0;
  for (unsigned i = 0, count = Octet(config, 22); i < count; ++i) {
    if (arrays.size() < 3) throw std::invalid_argument("Truncated HEVC configuration array");
    const auto flags = Octet(arrays);
    const auto type = flags & 63u;
    const auto nals = Big16(arrays, 1);
    const auto mask = std::uint64_t{1} << type;
    if (nals && (present & mask)) throw std::invalid_argument("Duplicate HEVC configuration array");
    if (nals) present |= mask;
    // hvc1 carries all parameter sets out of band. hev1 can also supply them in samples.
    if (hvc1 && type >= 32 && type <= 34 && !(flags & 128))
      throw std::invalid_argument("Incomplete hvc1 parameter-set array");
    arrays = arrays.subspan(3);
    for (unsigned j = 0; j < nals; ++j) {
      if (arrays.size() < 2) throw std::invalid_argument("Truncated HEVC configuration NAL length");
      const auto size = Big16(arrays);
      arrays = arrays.subspan(2);
      if (size > arrays.size() || HevcNalType(arrays.first(size)) != type)
        throw std::invalid_argument("Invalid HEVC configuration NAL unit");
      if (collect && type >= 32 && type <= 34) result.parameters.push_back(arrays.first(size));
      arrays = arrays.subspan(size);
    }
  }
  constexpr auto ParameterSets = std::uint64_t{7} << 32;
  if (!arrays.empty() || (hvc1 && (present & ParameterSets) != ParameterSets))
    throw std::invalid_argument("Invalid or incomplete HEVC decoder configuration");
  return result;
}

bool IsHevcIdr(HevcBytes bytes, unsigned length_size, bool hvc1, bool single_layer) {
  if (length_size != 1 && length_size != 2 && length_size != 4)
    throw std::invalid_argument("Invalid HEVC NAL length size");
  std::uint64_t picture_layers = 0;
  bool idr = true;
  while (!bytes.empty()) {
    if (bytes.size() < length_size) throw std::invalid_argument("Truncated HEVC NAL length");
    std::uint32_t length = 0;
    for (const auto octet : bytes.first(length_size)) length = (length << 8) | std::to_integer<unsigned>(octet);
    bytes = bytes.subspan(length_size);
    if (length > bytes.size()) throw std::invalid_argument("Invalid HEVC sample NAL length");
    const auto nal = bytes.first(length);
    const auto type = HevcNalType(nal);
    if (hvc1 && type >= 32 && type <= 34) throw std::invalid_argument("hvc1 sample contains in-band parameter sets");
    if (type < 32) {
      if (nal.size() < 3) throw std::invalid_argument("Truncated HEVC slice header");
      const auto layer = (Big16(nal) >> 3) & 63;
      if (single_layer && layer) throw std::invalid_argument("Multilayer HEVC is unsupported");
      const auto mask = std::uint64_t{1} << layer;
      const bool first_slice = (Octet(nal, 2) & 128) != 0;
      const bool started = (picture_layers & mask) != 0;
      // An access unit can start one picture per layer. Remaining slices belong to that picture.
      if (first_slice && started) throw std::invalid_argument("HEVC sample contains more than one picture in a layer");
      if (!first_slice && !started) throw std::invalid_argument("HEVC sample begins inside a coded picture");
      if (first_slice) {
        picture_layers |= mask;
        idr &= type == 19 || type == 20;
      }
    }
    bytes = bytes.subspan(length);
  }
  if (!picture_layers) throw std::invalid_argument("HEVC sample has no coded picture");
  return idr;
}

} // namespace huxerui::media::detail
