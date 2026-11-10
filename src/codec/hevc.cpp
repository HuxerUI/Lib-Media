#include "hevc.h"

#include <charconv>
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

class HevcBits {
public:
  explicit HevcBits(HevcBytes nal) : data_(nal.subspan(2)) {}
  unsigned Read(unsigned count) {
    if (count > 32) throw std::invalid_argument("Invalid HEVC field width");
    unsigned result = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!remaining_) {
        if (zeros_ == 2 && !data_.empty() && Octet(data_) == 3) {
          data_ = data_.subspan(1);
          zeros_ = 0;
        }
        if (data_.empty()) throw std::invalid_argument("Truncated HEVC syntax");
        byte_ = Octet(data_);
        data_ = data_.subspan(1);
        remaining_ = 8;
        zeros_ = byte_ ? 0 : zeros_ + 1;
      }
      result = (result << 1) | ((byte_ >> --remaining_) & 1u);
    }
    return result;
  }
  unsigned Unsigned() {
    unsigned zeros = 0;
    while (!Read(1)) if (++zeros > 30) throw std::invalid_argument("HEVC syntax exceeds integer bounds");
    return (1u << zeros) - 1 + Read(zeros);
  }
private:
  HevcBytes data_;
  unsigned zeros_ = 0, byte_ = 0, remaining_ = 0;
};

} // namespace

unsigned HevcNalType(HevcBytes nal) {
  if (nal.size() < 2) throw std::invalid_argument("Truncated HEVC NAL header");
  const auto header = Big16(nal);
  if ((header & 0x8000) || !(header & 7)) throw std::invalid_argument("Invalid HEVC NAL header");
  return (header >> 9) & 63;
}

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

HevcSequence ReadHevcSequence(HevcBytes sps) {
  if (HevcNalType(sps) != 33 || (Big16(sps) & 0x01f8))
    throw std::invalid_argument("Expected a single-layer HEVC SPS");
  HevcBits bits(sps);
  bits.Read(4);
  const auto sub_layers = bits.Read(3);
  if (sub_layers > 6) throw std::invalid_argument("Invalid HEVC temporal layer count");
  HevcSequence info{};
  info.temporal_layers = sub_layers + 1;
  info.temporal_id_nested = bits.Read(1) != 0;
  for (auto& byte : info.profile) byte = std::byte(bits.Read(8));
  std::array<unsigned, 6> flags{};
  for (unsigned i = 0; i < sub_layers; ++i) flags[i] = bits.Read(2);
  if (sub_layers) bits.Read((8 - sub_layers) * 2);
  for (unsigned i = 0; i < sub_layers; ++i) {
    if (flags[i] & 2) { bits.Read(32); bits.Read(32); bits.Read(24); }
    if (flags[i] & 1) bits.Read(8);
  }
  if (bits.Unsigned() > 15) throw std::invalid_argument("Invalid HEVC SPS ID");
  info.chroma_format = bits.Unsigned();
  if (info.chroma_format != 1) throw std::invalid_argument("HEVC sequence requires 4:2:0 chroma");
  info.width = bits.Unsigned();
  info.height = bits.Unsigned();
  unsigned right = 0, bottom = 0;
  if (bits.Read(1)) {
    info.left = bits.Unsigned() * 2;
    right = bits.Unsigned() * 2;
    info.top = bits.Unsigned() * 2;
    bottom = bits.Unsigned() * 2;
  }
  if (std::uint64_t(info.left) + right >= info.width || std::uint64_t(info.top) + bottom >= info.height)
    throw std::invalid_argument("Invalid HEVC conformance window");
  info.visible_width = info.width - info.left - right;
  info.visible_height = info.height - info.top - bottom;
  info.luma_bits = 8 + bits.Unsigned();
  info.chroma_bits = 8 + bits.Unsigned();
  return info;
}

bool HevcSliceIsB(HevcBytes slice, const std::vector<HevcBytes>& parameters) {
  const auto type = HevcNalType(slice);
  HevcBits bits(slice);
  if (!bits.Read(1)) throw std::invalid_argument("Expected the first HEVC slice of the picture");
  if (type >= 16 && type <= 23) bits.Read(1);
  const auto pps_id = bits.Unsigned();
  for (const auto pps : parameters) {
    if (HevcNalType(pps) != 34) continue;
    HevcBits header(pps);
    if (header.Unsigned() != pps_id) continue;
    header.Unsigned();
    header.Read(2);
    bits.Read(header.Read(3));
    const auto slice_type = bits.Unsigned();
    if (slice_type > 2) throw std::invalid_argument("Invalid HEVC slice type");
    return slice_type == 0;
  }
  throw std::invalid_argument("HEVC slice references an unknown PPS");
}

std::vector<std::byte> HevcAnnexBConfiguration(HevcBytes config) {
  std::vector<std::byte> bytes;
  for (auto nal : ParseHevcConfiguration(config, true, true).parameters) {
    bytes.insert(bytes.end(), {std::byte{}, std::byte{}, std::byte{}, std::byte{1}});
    bytes.insert(bytes.end(), nal.begin(), nal.end());
  }
  return bytes;
}

std::string HevcCodecString(HevcBytes config) {
  ParseHevcConfiguration(config, true, false);
  const auto profile = Octet(config, 1);
  std::string codec = "hvc1.";
  if (const auto space = profile >> 6) codec += char('A' + space - 1);
  codec += std::to_string(profile & 31u) + ".";
  std::uint32_t compatibility = 0;
  for (unsigned bit = 0; bit < 32; ++bit)
    compatibility |= ((Octet(config, 2 + bit / 8) >> (7 - bit % 8)) & 1u) << bit;
  char flags[8];
  const auto hex = std::to_chars(flags, flags + sizeof(flags), compatibility, 16);
  codec.append(flags, hex.ptr);
  codec += profile & 32 ? ".H" : ".L";
  codec += std::to_string(Octet(config, 12));
  std::size_t end = 12;
  while (end > 6 && !Octet(config, end - 1)) --end;
  constexpr char Hex[] = "0123456789ABCDEF";
  for (std::size_t i = 6; i < end; ++i) {
    const auto value = Octet(config, i);
    codec += '.';
    codec += Hex[value >> 4];
    codec += Hex[value & 15];
  }
  return codec;
}

} // namespace huxerui::media::detail
