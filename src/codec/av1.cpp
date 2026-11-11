#include "av1.h"

#include <cstdint>
#include <stdexcept>

namespace huxerui::media::detail {
namespace {

constexpr unsigned SequenceHeader = 1;
constexpr unsigned TemporalDelimiter = 2;
constexpr unsigned FrameHeader = 3;
constexpr unsigned Frame = 6;

unsigned Octet(std::byte byte) { return std::to_integer<unsigned>(byte); }

class Bits {
public:
  explicit Bits(Av1Bytes bytes) : bytes_(bytes) {}

  unsigned Read(unsigned count) {
    if (count > bytes_.size() * 8 - position_) throw std::invalid_argument("Truncated AV1 header");
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position_)
      value = (value << 1) | ((Octet(bytes_[position_ / 8]) >> (7 - position_ % 8)) & 1);
    return value;
  }

  void SkipUvlc() {
    unsigned zeros = 0;
    while (zeros < 32 && !Read(1)) ++zeros;
    if (zeros < 32) Read(zeros);
  }

private:
  Av1Bytes bytes_;
  std::size_t position_ = 0;
};

struct Obu {
  unsigned type;
  unsigned spatial_id;
  bool has_size;
  Av1Bytes payload;
};

Obu ReadObu(Av1Bytes& bytes) {
  const auto header = Octet(bytes.front());
  bytes = bytes.subspan(1);
  if (header & 0x81) throw std::invalid_argument("Invalid AV1 OBU header");
  const auto type = (header >> 3) & 15;
  unsigned spatial_id = 0;
  if (header & 4) {
    if (bytes.empty() || (Octet(bytes.front()) & 7)) throw std::invalid_argument("Invalid AV1 OBU extension");
    spatial_id = (Octet(bytes.front()) >> 3) & 3;
    bytes = bytes.subspan(1);
  }
  if (!(header & 2)) {
    const auto payload = bytes;
    bytes = {};
    return {type, spatial_id, false, payload};
  }
  std::uint64_t size = 0;
  for (unsigned shift = 0; shift < 56 && !bytes.empty(); shift += 7) {
    const auto next = Octet(bytes.front());
    bytes = bytes.subspan(1);
    size |= std::uint64_t(next & 127) << shift;
    if (!(next & 128)) {
      if (size > bytes.size() || size > UINT32_MAX) throw std::invalid_argument("AV1 OBU exceeds its packet");
      const auto payload = bytes.first(static_cast<std::size_t>(size));
      bytes = bytes.subspan(payload.size());
      return {type, spatial_id, true, payload};
    }
  }
  throw std::invalid_argument("Invalid AV1 OBU length");
}

Av1Sequence ReadSequence(Av1Bytes payload) {
  Bits bits(payload);
  const auto profile = bits.Read(3);
  const auto still = bits.Read(1);
  const bool reduced = bits.Read(1);
  if (profile > 2 || (reduced && !still)) throw std::invalid_argument("Invalid AV1 sequence header");
  if (reduced) {
    bits.Read(5);
  } else {
    unsigned delay_bits = 0;
    if (bits.Read(1)) {
      bits.Read(32);
      bits.Read(32);
      if (bits.Read(1)) bits.SkipUvlc();
      if (bits.Read(1)) {
        delay_bits = bits.Read(5) + 1;
        bits.Read(32);
        bits.Read(10);
      }
    }
    const bool initial_delay = bits.Read(1);
    const auto operating_points = bits.Read(5) + 1;
    for (unsigned i = 0; i < operating_points; ++i) {
      bits.Read(12);
      if (bits.Read(5) > 7) bits.Read(1);
      if (delay_bits && bits.Read(1)) {
        bits.Read(delay_bits);
        bits.Read(delay_bits);
        bits.Read(1);
      }
      if (initial_delay && bits.Read(1)) bits.Read(4);
    }
  }
  const auto width_bits = bits.Read(4) + 1;
  const auto height_bits = bits.Read(4) + 1;
  const auto width = bits.Read(width_bits) + 1;
  const auto height = bits.Read(height_bits) + 1;
  if (!reduced && bits.Read(1)) bits.Read(7);
  bits.Read(3);
  if (!reduced) {
    bits.Read(4);
    const bool order_hint = bits.Read(1);
    if (order_hint) bits.Read(2);
    const bool choose_screen_tools = bits.Read(1);
    const bool screen_tools = choose_screen_tools || bits.Read(1);
    if (screen_tools && !bits.Read(1)) bits.Read(1);
    if (order_hint) bits.Read(3);
  }
  bits.Read(3);
  const bool high_bitdepth = bits.Read(1);
  if (profile == 2 && high_bitdepth) bits.Read(1);
  const bool monochrome = profile != 1 && bits.Read(1);
  unsigned primaries = 2, transfer = 2, matrix = 2;
  if (bits.Read(1)) {
    primaries = bits.Read(8);
    transfer = bits.Read(8);
    matrix = bits.Read(8);
  }
  const bool rgb = primaries == 1 && transfer == 13 && matrix == 0;
  return {width, height, reduced, profile == 0 && !high_bitdepth && !monochrome && !rgb && transfer != 16 && transfer != 18};
}

} // namespace

Av1Configuration ParseAv1Configuration(Av1Bytes bytes) {
  if (bytes.size() < 4 || bytes.size() > 65536 || Octet(bytes[0]) != 0x81)
    throw std::invalid_argument("Invalid AV1 codec configuration record");
  const auto profile = Octet(bytes[1]) >> 5;
  const auto level = Octet(bytes[1]) & 31;
  const auto color = Octet(bytes[2]);
  const auto delay = Octet(bytes[3]);
  if (profile > 2 || (delay & 0xe0) || (!(delay & 0x10) && (delay & 15)))
    throw std::invalid_argument("Invalid AV1 codec configuration fields");
  Av1Configuration config{profile, level, bool(color & 0x80), (color & 0x7c) == 0x0c, {}};
  for (auto obus = bytes.subspan(4); !obus.empty();) {
    const auto obu = ReadObu(obus);
    if (!obu.has_size || obu.type == FrameHeader || obu.type == Frame)
      throw std::invalid_argument("Invalid AV1 configuration OBU");
    if (obu.type == SequenceHeader) {
      if (config.sequence) throw std::invalid_argument("Duplicate AV1 configuration sequence header");
      config.sequence = ReadSequence(obu.payload);
    }
  }
  return config;
}

Av1PacketInfo InspectAv1Packet(Av1Bytes bytes) {
  Av1PacketInfo result;
  bool frame_seen = false;
  auto obus = bytes;
  while (!obus.empty()) {
    const auto offset = bytes.size() - obus.size();
    const auto obu = ReadObu(obus);
    if (!obu.has_size) result.unsized_obu = offset;
    result.spatial_layers |= obu.spatial_id != 0;
    if (obu.type == SequenceHeader) {
      if (frame_seen && !result.sequence) throw std::invalid_argument("Misplaced AV1 sequence header");
      const auto sequence = ReadSequence(obu.payload);
      // Repeated headers may vary their operating parameters within the same sequence.
      if (result.sequence && *result.sequence != sequence)
        throw std::invalid_argument("AV1 sequence parameters change within a temporal unit");
      result.sequence = sequence;
    } else if (obu.type == FrameHeader || obu.type == Frame) {
      if (obu.payload.empty()) throw std::invalid_argument("Empty AV1 frame header");
      const bool reduced = result.sequence && result.sequence->reduced_header;
      Bits bits(obu.payload);
      const bool existing = !reduced && bits.Read(1);
      const bool key = reduced || (!existing && bits.Read(2) == 0);
      const bool show = reduced || existing || bits.Read(1);
      if (!frame_seen) result.sync = result.sequence && key && show;
      frame_seen = true;
      result.pictures += show;
    } else if (obu.type == TemporalDelimiter && offset) {
      throw std::invalid_argument("AV1 packet contains multiple temporal units");
    }
  }
  if (!frame_seen) throw std::invalid_argument("AV1 temporal unit contains no frame");
  return result;
}

} // namespace huxerui::media::detail
