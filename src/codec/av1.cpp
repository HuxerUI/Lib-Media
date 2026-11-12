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
  unsigned level = 0;
  bool high_tier = false;
  if (reduced) {
    level = bits.Read(5);
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
      const auto point_level = bits.Read(5);
      const bool tier = point_level > 7 && bits.Read(1);
      if (!i) { level = point_level; high_tier = tier; }
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
  return {width, height, reduced, profile == 0 && !monochrome && !rgb && transfer != 16 && transfer != 18,
          high_bitdepth ? 10u : 8u, level, high_tier};
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
  Av1Configuration config{profile, level, bool(color & 0x80), (color & 0x3c) == 0x0c, color & 0x40 ? 10u : 8u, {}};
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

std::vector<std::byte> MakeAv1Configuration(Av1Bytes header) {
  auto bytes = header;
  const auto obu = ReadObu(bytes);
  if (obu.type != SequenceHeader || !obu.has_size || !bytes.empty())
    throw std::invalid_argument("Expected one sized AV1 sequence header");
  const auto sequence = ReadSequence(obu.payload);
  if (!sequence.sdr_420) throw std::invalid_argument("Expected an AV1 Main SDR sequence");
  std::vector record{std::byte{0x81}, std::byte(sequence.level),
      std::byte((sequence.high_tier ? 0x80 : 0) | (sequence.bit_depth == 10 ? 0x40 : 0) | 0x0c), std::byte{}};
  record.insert(record.end(), header.begin(), header.end());
  return record;
}

std::vector<std::byte> SizedAv1Packet(Av1Bytes bytes, std::size_t offset) {
  const auto header_size = (std::to_integer<unsigned>(bytes[offset]) & 4) ? 2u : 1u;
  const auto payload = bytes.subspan(offset + header_size);
  std::vector<std::byte> packet(bytes.begin(), bytes.begin() + offset + header_size);
  packet[offset] |= std::byte{2};
  auto size = payload.size();
  do {
    const auto digit = size & 127;
    size >>= 7;
    packet.push_back(static_cast<std::byte>(digit | (size ? 128 : 0)));
  } while (size);
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
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
      if (result.sequence && (result.sequence->width != sequence.width || result.sequence->height != sequence.height ||
          result.sequence->reduced_header != sequence.reduced_header || result.sequence->sdr_420 != sequence.sdr_420 ||
          result.sequence->bit_depth != sequence.bit_depth))
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
