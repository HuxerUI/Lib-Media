#include "framed_audio.h"

#include <array>

namespace huxerui::media::detail {
namespace {

constexpr auto Bad = ContainerErrorCode::InvalidData;

unsigned Crc(ByteView bytes, unsigned polynomial, unsigned width) {
  const auto top = 1u << width;
  unsigned crc = 0;
  for (auto byte : bytes) {
    crc ^= std::to_integer<unsigned>(byte) << (width - 8);
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc <<= 1;
      if (crc & top) crc ^= top | polynomial;
    }
  }
  return crc;
}

struct StreamInfo {
  unsigned min_block, max_block, max_frame, rate, channels, bits;
  std::uint64_t total;
};

StreamInfo Info(ByteView bytes, ContainerErrorCode code) {
  if (bytes.size() != 34) Fail(code, "FLAC STREAMINFO must be 34 bytes");
  const auto packed = Number<std::uint64_t>(bytes, 10);
  const auto result = StreamInfo{Number<std::uint16_t>(bytes), Number<std::uint16_t>(bytes, 2),
      Number<std::uint32_t>(bytes, 6) & 0xffffffu, unsigned(packed >> 44), unsigned((packed >> 41) & 7) + 1,
      unsigned((packed >> 36) & 31) + 1, packed & ((std::uint64_t{1} << 36) - 1)};
  if (result.min_block < 16 || result.max_block < result.min_block || !result.rate || result.rate > 655350 ||
      result.bits < 4) Fail(code, "Invalid FLAC STREAMINFO geometry");
  return result;
}

class FrameBits {
public:
  explicit FrameBits(ContainerInput& input, std::size_t limit) : input_(input), limit_(limit) {}
  unsigned Get(unsigned count) {
    Require(count);
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position_)
      value = (value << 1) | ((std::to_integer<unsigned>(bytes_[position_ / 8]) >> (7 - position_ % 8)) & 1);
    return value;
  }
  void Skip(std::size_t count) {
    Require(count);
    position_ += count;
  }
  void Unary() {
    while (!Get(1)) {}
  }
  unsigned Wasted(unsigned width) {
    for (unsigned n = 1; n < width; ++n) if (Get(1)) return n;
    Fail(Bad, "FLAC wasted bits exceed the channel width");
  }
  std::size_t BytesUsed() const { return position_ / 8; }
  ByteView BytesRead() const { return bytes_.first(BytesUsed()); }
  void Align() {
    if (position_ % 8 && Get(8 - position_ % 8)) Fail(Bad, "Nonzero FLAC frame padding");
  }
private:
  void Require(std::size_t count) {
    if (count > limit_ * 8 - position_)
      Fail(ContainerErrorCode::LimitExceeded, "FLAC frame exceeds its size limit");
    const auto needed = (position_ + count + 7) / 8;
    if (needed <= bytes_.size()) return;
    bytes_ = input_.Peek(std::min(limit_, (needed + 4095) / 4096 * 4096));
    if (needed > bytes_.size()) Fail(Bad, "Truncated FLAC frame");
  }
  ContainerInput& input_;
  std::size_t limit_;
  std::size_t position_ = 0;
  ByteView bytes_;
};

std::uint64_t FrameNumber(FrameBits& bits) {
  const auto first = bits.Get(8);
  if (first < 128) return first;
  const unsigned size = std::countl_one(static_cast<std::uint8_t>(first));
  if (size < 2 || size > 7) Fail(Bad, "Invalid FLAC coded frame number");
  std::uint64_t number = first & (127u >> size);
  for (unsigned i = 1; i < size; ++i) {
    const auto value = bits.Get(8);
    if ((value & 192) != 128) Fail(Bad, "Invalid FLAC coded frame number continuation");
    number = (number << 6) | (value & 63);
  }
  const auto minimum_bits = size == 2 ? 7u : 5u * size - 4;
  if (number < (std::uint64_t{1} << minimum_bits)) Fail(Bad, "Overlong FLAC coded frame number");
  return number;
}

void Residual(FrameBits& bits, unsigned block, unsigned warmup) {
  const auto method = bits.Get(2);
  if (method > 1) Fail(Bad, "Reserved FLAC residual method");
  const auto partitions = 1u << bits.Get(4);
  const auto per_partition = block / partitions;
  if (block % partitions || per_partition < warmup) Fail(Bad, "Invalid FLAC residual partition size");
  const auto rice_width = 4 + method;
  const auto escape = (1u << rice_width) - 1;
  for (unsigned p = 0; p < partitions; ++p) {
    const auto parameter = bits.Get(rice_width);
    const auto count = per_partition - (p == 0 ? warmup : 0);
    if (parameter == escape) {
      bits.Skip(std::size_t(count) * bits.Get(5));
    } else {
      for (unsigned i = 0; i < count; ++i) {
        bits.Unary();
        bits.Skip(parameter);
      }
    }
  }
}

void Subframe(FrameBits& bits, unsigned block, unsigned width) {
  if (bits.Get(1)) Fail(Bad, "Invalid FLAC subframe padding");
  const auto type = bits.Get(6);
  if (bits.Get(1)) width -= bits.Wasted(width);
  if (type <= 1) {
    bits.Skip(std::size_t(type ? block : 1) * width);
    return;
  }
  const bool lpc = type >= 32;
  if (!lpc && (type < 8 || type > 12)) Fail(Bad, "Reserved FLAC subframe type");
  const auto order = lpc ? type - 31 : type - 8;
  if (order > block) Fail(Bad, "FLAC predictor exceeds the block size");
  bits.Skip(std::size_t(order) * width);
  if (lpc) {
    const auto precision = bits.Get(4) + 1;
    if (precision == 16) Fail(Bad, "Reserved FLAC LPC precision");
    bits.Skip(5 + std::size_t(order) * precision);
  }
  Residual(bits, block, order);
}

AudioPacket ReadFlacFrame(ContainerInput& input, const StreamInfo& info, std::int64_t position) {
  FrameBits bits(input, info.max_frame ? info.max_frame : MaxContainerPacket);
  if (bits.Get(15) != 0x7ffc) Fail(Bad, "Invalid FLAC frame sync");
  const auto variable = bits.Get(1);
  const auto block_code = bits.Get(4);
  const auto rate_code = bits.Get(4);
  const auto channels = bits.Get(4);
  const auto size_code = bits.Get(3);
  if (bits.Get(1) || !block_code || rate_code == 15 || channels > 10 || size_code == 3)
    Fail(Bad, "Reserved FLAC frame header field");
  const auto number = FrameNumber(bits);
  unsigned block;
  if (block_code == 1) block = 192;
  else if (block_code <= 5) block = 576u << (block_code - 2);
  else if (block_code <= 7) block = bits.Get(block_code == 6 ? 8 : 16) + 1;
  else block = 256u << (block_code - 8);
  constexpr std::array rates{0u, 88200u, 176400u, 192000u, 8000u, 16000u, 22050u, 24000u,
                             32000u, 44100u, 48000u, 96000u};
  const auto rate = rate_code == 0 ? info.rate : rate_code < 12 ? rates[rate_code] :
      bits.Get(rate_code == 12 ? 8 : 16) * (rate_code == 12 ? 1000 : rate_code == 14 ? 10 : 1);
  constexpr std::array widths{0u, 8u, 12u, 0u, 16u, 20u, 24u, 32u};
  const auto width = size_code ? widths[size_code] : info.bits;
  if ((channels < 8 ? channels + 1 : 2) != info.channels || rate != info.rate || width != info.bits ||
      block > info.max_block || (variable ? number != std::uint64_t(position) :
      number > 0x7fffffffu || number * info.max_block != std::uint64_t(position)))
    Fail(Bad, "FLAC frame does not follow its stream geometry or sample position");
  const auto header_crc = bits.Get(8);
  if (Crc(bits.BytesRead().first(bits.BytesUsed() - 1), 0x07, 8) != header_crc)
    Fail(Bad, "FLAC frame header checksum differs");
  for (unsigned channel = 0; channel < info.channels; ++channel) {
    const bool side = (channels == 8 && channel == 1) || (channels == 9 && channel == 0) ||
                      (channels == 10 && channel == 1);
    Subframe(bits, block, width + side);
  }
  bits.Align();
  const auto checksum = bits.Get(16);
  if (Crc(bits.BytesRead().first(bits.BytesUsed() - 2), 0x8005, 16) != checksum)
    Fail(Bad, "FLAC frame checksum differs");
  return {input.ReadBytes(bits.BytesUsed()), block};
}

class FlacReader final : public FramedAudioReader {
public:
  explicit FlacReader(ContainerInput input) : FramedAudioReader(std::move(input), ContainerFormat::Flac) {
    this->input.Skip(4);
    Bytes stream_info;
    std::size_t metadata = 0;
    bool last = false;
    while (!last) {
      const auto header = this->input.ReadBytes(4);
      const auto kind = Number<std::uint8_t>(header);
      last = kind & 128;
      const auto type = kind & 127;
      const auto size = Number<std::uint32_t>(header) & 0xffffffu;
      if (metadata > MaxContainerMetadata - size - 4)
        Fail(ContainerErrorCode::LimitExceeded, "FLAC metadata exceeds its size limit");
      if (!metadata) {
        if (type || size != 34) Fail(Bad, "FLAC STREAMINFO must be first");
        stream_info = this->input.ReadBytes(size);
      } else {
        if (!type || type == 127) Fail(Bad, "Invalid FLAC metadata block type");
        this->input.Skip(size);
      }
      metadata += size + 4;
    }
    info_ = Info(stream_info, Bad);
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .codec = CodecId::Flac,
                     .initialization_data = std::move(stream_info), .timescale = info_.rate,
                     .sample_rate = info_.rate, .channel_count = info_.channels};
    Begin(std::move(track), info_.total ? std::optional{info_.total} : std::nullopt);
  }

private:
  std::optional<AudioPacket> ReadFrame(std::int64_t position) override {
    if (input.Peek(1).empty()) return {};
    return ReadFlacFrame(input, info_, position);
  }
  StreamInfo info_{};
};
} // namespace

std::unique_ptr<ContainerReader> OpenFlacReader(ContainerInput input) {
  return std::make_unique<FlacReader>(std::move(input));
}
} // namespace huxerui::media::detail
