#include "pcm.h"

#include <array>

namespace huxerui::media::detail {
namespace {

constexpr std::array<std::uint8_t, 12> Wave64RiffTail{0x2e, 0x91, 0xcf, 0x11, 0xa5, 0xd6, 0x28, 0xdb, 4, 0xc1, 0, 0};
constexpr std::array<std::uint8_t, 12> Wave64ChunkTail{0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1, 0, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
constexpr std::uint64_t Wave64HeaderBytes = 24;

void PutWave64Guid(Bytes& bytes, std::uint32_t tag, bool riff = false) {
  Put(bytes, tag);
  Append(bytes, std::as_bytes(std::span(riff ? Wave64RiffTail : Wave64ChunkTail)));
}

std::uint64_t Wave64Padding(std::uint64_t size) { return (8 - size % 8) % 8; }

unsigned Bits(CodecId codec) {
  switch (codec) {
    case CodecId::PcmU8: return 8;
    case CodecId::PcmS16Le: return 16;
    case CodecId::PcmS24Le: return 24;
    case CodecId::PcmS32Le:
    case CodecId::PcmF32Le: return 32;
    default: Fail(ContainerErrorCode::Unsupported, "Unsupported WAVE PCM coding");
  }
}

CodecId PcmCodec(unsigned format, unsigned bits) {
  if (format == 1) {
    switch (bits) {
      case 8: return CodecId::PcmU8;
      case 16: return CodecId::PcmS16Le;
      case 24: return CodecId::PcmS24Le;
      case 32: return CodecId::PcmS32Le;
    }
  }
  if (format == 3 && bits == 32) return CodecId::PcmF32Le;
  Fail(ContainerErrorCode::Unsupported, "Unsupported WAVE sample format");
}

constexpr std::array<std::byte, 12> WaveGuidTail{
    std::byte{0}, std::byte{0}, std::byte{16}, std::byte{0}, std::byte{128}, std::byte{0},
    std::byte{0}, std::byte{170}, std::byte{0}, std::byte{56}, std::byte{155}, std::byte{113}};

unsigned ExtensibleFormat(ByteView suffix, unsigned bits, unsigned channels) {
  if (suffix.size() != 22 || !std::ranges::equal(Slice(suffix, 10, 12), WaveGuidTail))
    Fail(ContainerErrorCode::Unsupported, "Unsupported WAVE extensible format GUID");
  const auto valid_bits = Number<std::uint16_t>(suffix, 0, true);
  const auto mask = Number<std::uint32_t>(suffix, 2, true);
  if (!valid_bits || valid_bits > bits || (mask && std::popcount(mask) != static_cast<int>(channels)))
    Fail(ContainerErrorCode::InvalidData, "Invalid WAVE extensible sample precision or channel mask");
  return Number<std::uint32_t>(suffix, 6, true);
}

void ReadFormat(ByteView bytes, MediaTrack& track, unsigned& alignment) {
  auto& description = track.descriptions.front();
  auto format = Number<std::uint16_t>(bytes, 0, true);
  const auto channels = Number<std::uint16_t>(bytes, 2, true);
  const auto rate = Number<std::uint32_t>(bytes, 4, true);
  const auto byte_rate = Number<std::uint32_t>(bytes, 8, true);
  alignment = Number<std::uint16_t>(bytes, 12, true);
  const auto bits = Number<std::uint16_t>(bytes, 14, true);
  if (format == 0xfffe) {
    if (Number<std::uint16_t>(bytes, 16, true) < 22)
      Fail(ContainerErrorCode::InvalidData, "Short WAVE extensible format");
    const auto suffix = Slice(bytes, 18, 22);
    format = ExtensibleFormat(suffix, bits, channels);
    description.initialization_data.assign(suffix.begin(), suffix.end());
    description.speaker_mask = Number<std::uint32_t>(suffix, 2, true);
  }
  description.codec = PcmCodec(format, bits);
  if (!channels || !rate || alignment != channels * (bits / 8) ||
      byte_rate != static_cast<std::uint64_t>(alignment) * rate)
    Fail(ContainerErrorCode::InvalidData, "Inconsistent WAVE sample rate, byte rate, or frame alignment");
  track.id = 1;
  track.kind = TrackKind::Audio;
  description.sample_rate = track.timescale = rate;
  description.channel_count = channels;
}

class WaveReader final : public PcmReader {
public:
  WaveReader(ContainerInput input, ContainerFormat format) : PcmReader(std::move(input), format) {
    if (format == ContainerFormat::Wave64) {
      ReadWave64();
      return;
    }
    const auto header = this->input.ReadBytes(12);
    const bool rf64 = format == ContainerFormat::Rf64;
    auto end = AddOffset(8, Number<std::uint32_t>(header, 4, true));
    std::uint64_t rf64_data = 0;
    if (rf64) {
      if (end != std::uint64_t{0xffffffff} + 8)
        Fail(ContainerErrorCode::InvalidData, "RF64 requires the RIFF size sentinel");
      auto chunk = this->input.ReadBytes(8);
      const auto size = Number<std::uint32_t>(chunk, 4, true);
      if (Number<std::uint32_t>(chunk) != Tag("ds64") || size < 28)
        Fail(ContainerErrorCode::InvalidData, "RF64 requires a ds64 chunk");
      auto sizes = this->input.ReadBytes(28);
      end = AddOffset(8, Number<std::uint64_t>(sizes, 0, true));
      rf64_data = Number<std::uint64_t>(sizes, 8, true);
      if (Number<std::uint32_t>(sizes, 24, true))
        Fail(ContainerErrorCode::Unsupported, "RF64 additional 64-bit chunk sizes are unsupported");
      if (size > MaxContainerMetadata || AddOffset(this->input.Position(), size - 28 + (size & 1)) > end)
        Fail(ContainerErrorCode::InvalidData, "Invalid RF64 size table");
      this->input.Skip(size - 28 + (size & 1));
    }
    if (end < this->input.Position() || (this->input.Size() && end > *this->input.Size()))
      Fail(ContainerErrorCode::InvalidData, "WAVE length exceeds input");

    MediaTrack track{.descriptions = {{}}};
    unsigned alignment = 0;
    std::size_t chunks = 0;
    while (end - this->input.Position() >= 8) {
      if (++chunks > 4096 || (!this->input.Random() && this->input.Position() > MaxContainerMetadata))
        Fail(ContainerErrorCode::LimitExceeded, "WAVE chunk discovery exceeds its budget");
      auto chunk = this->input.ReadBytes(8);
      const auto type = Number<std::uint32_t>(chunk);
      std::uint64_t size = Number<std::uint32_t>(chunk, 4, true);
      if (rf64 && type == Tag("data") && size == 0xffffffff) size = rf64_data;
      const auto padded = AddOffset(size, size & 1);
      if (padded > end - this->input.Position()) Fail(ContainerErrorCode::InvalidData, "WAVE chunk exceeds RIFF length");
      if (type == Tag("fmt ")) {
        if (alignment || size < 16 || size > 65536) Fail(ContainerErrorCode::InvalidData, "Invalid WAVE format chunk");
        ReadFormat(this->input.ReadBytes(static_cast<std::size_t>(size)), track, alignment);
        this->input.Skip(size & 1);
      } else if (type == Tag("data")) {
        Begin(std::move(track), size, alignment, static_cast<unsigned>(size & 1));
        return;
      } else {
        if ((!this->input.Random() || !this->input.Size()) && padded > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "WAVE header skip exceeds its budget");
        this->input.Skip(padded);
      }
    }
    Fail(ContainerErrorCode::InvalidData, "Missing WAVE data chunk");
  }

private:
  void ReadWave64() {
    constexpr auto invalid = ContainerErrorCode::InvalidData;
    const auto header = input.ReadBytes(40);
    const auto riff = std::as_bytes(std::span(Wave64RiffTail));
    const auto tail = std::as_bytes(std::span(Wave64ChunkTail));
    const auto end = Number<std::uint64_t>(header, 16, true);
    if (Number<std::uint32_t>(header) != Tag("riff") || !std::ranges::equal(Slice(header, 4, 12), riff) ||
        Number<std::uint32_t>(header, 24) != Tag("wave") || !std::ranges::equal(Slice(header, 28, 12), tail) ||
        end < header.size() || (input.Size() && end > *input.Size()))
      Fail(invalid, "Invalid Wave64 RIFF header");

    MediaTrack track{.descriptions = {{}}};
    std::optional<std::uint64_t> fact;
    unsigned alignment = 0;
    std::size_t chunks = 0;
    while (end - input.Position() >= Wave64HeaderBytes) {
      if (++chunks > 4096 || (!input.Random() && input.Position() > MaxContainerMetadata))
        Fail(ContainerErrorCode::LimitExceeded, "Wave64 chunk discovery exceeds its budget");
      const auto chunk = input.ReadBytes(Wave64HeaderBytes);
      const auto size = Number<std::uint64_t>(chunk, 16, true);
      const auto padded = AddOffset(size, Wave64Padding(size));
      if (size < Wave64HeaderBytes || padded - Wave64HeaderBytes > end - input.Position())
        Fail(invalid, "Wave64 chunk exceeds the RIFF bounds");
      const auto payload = size - Wave64HeaderBytes;
      const auto type = std::ranges::equal(Slice(chunk, 4, 12), tail) ? Number<std::uint32_t>(chunk) : 0;
      if (type == Tag("fmt ")) {
        if (alignment || payload < 16 || payload > 65536) Fail(invalid, "Invalid Wave64 format chunk");
        ReadFormat(input.ReadBytes(static_cast<std::size_t>(payload)), track, alignment);
        input.Skip(padded - size);
      } else if (type == Tag("fact")) {
        if (fact || payload < 8 || payload > 65536) Fail(invalid, "Invalid Wave64 fact chunk");
        fact = Number<std::uint64_t>(input.ReadBytes(8), 0, true);
        input.Skip(padded - Wave64HeaderBytes - 8);
      } else if (type == Tag("data")) {
        if (!alignment) Fail(invalid, "Wave64 data precedes its format");
        auto bytes = payload;
        if (fact && *fact) {
          if (*fact > payload / alignment || payload - *fact * alignment > 7)
            Fail(invalid, "Wave64 sample count differs from its data length");
          bytes = *fact * alignment;
        } else if (payload % 8 == 0 && payload % alignment <= 7) {
          // Some writers include alignment bytes in the declared chunk size.
          bytes -= payload % alignment;
        }
        Begin(std::move(track), bytes, alignment, static_cast<unsigned>(padded - Wave64HeaderBytes - bytes));
        return;
      } else {
        if ((!input.Random() || !input.Size()) && padded - Wave64HeaderBytes > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "Wave64 header skip exceeds its budget");
        input.Skip(padded - Wave64HeaderBytes);
      }
    }
    Fail(invalid, "Missing Wave64 data chunk");
  }
};

class WaveWriter final : public ContainerWriter {
public:
  WaveWriter(ContainerOutput output, ContainerFormat format, std::span<const MediaTrack> tracks)
      : ContainerWriter(std::move(output), tracks), rf64_(format == ContainerFormat::Rf64), wave64_(format == ContainerFormat::Wave64) {
    const auto& track = tracks[0];
    const auto& description = track.descriptions.front();
    const auto bits = Bits(description.codec);
    const auto channels = *description.channel_count;
    const auto rate = *description.sample_rate;
    alignment_ = channels * (bits / 8);
    const std::uint16_t coding = description.codec == CodecId::PcmF32Le ? 3 : 1;
    const bool extensible = channels > 2 || description.speaker_mask || !description.initialization_data.empty();
    Bytes header;
    if (wave64_) {
      PutWave64Guid(header, Tag("riff"), true);
      Put<std::uint64_t>(header, 0, true);
      PutWave64Guid(header, Tag("wave"));
    } else {
      Put(header, Tag(rf64_ ? "RF64" : "RIFF"));
      Put(header, rf64_ ? 0xffffffffu : 0u, true);
      Put(header, Tag("WAVE"));
    }
    if (rf64_) {
      Put(header, Tag("ds64"));
      Put(header, 28u, true);
      header.resize(header.size() + 28);
    }
    const auto format_bytes = extensible ? 40u : (coding == 3 ? 18u : 16u);
    PutChunk(header, Tag("fmt "), format_bytes);
    Put<std::uint16_t>(header, extensible ? 0xfffe : coding, true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(channels), true);
    Put(header, rate, true);
    Put(header, rate * alignment_, true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(alignment_), true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(bits), true);
    if (extensible) {
      Put<std::uint16_t>(header, 22, true);
      if (description.initialization_data.empty()) {
        Put<std::uint16_t>(header, static_cast<std::uint16_t>(bits), true);
        Put(header, description.speaker_mask.value_or(0), true);
        Put<std::uint32_t>(header, coding, true);
        Append(header, WaveGuidTail);
      } else {
        Append(header, description.initialization_data);
      }
    } else if (coding == 3) {
      Put<std::uint16_t>(header, 0, true);
    }
    if (wave64_) header.resize(header.size() + Wave64Padding(format_bytes));
    if (wave64_ || coding == 3) {
      const auto fact_bytes = wave64_ ? 8u : 4u;
      PutChunk(header, Tag("fact"), fact_bytes);
      fact_offset_ = header.size();
      header.resize(header.size() + fact_bytes);
    }
    PutChunk(header, Tag("data"), rf64_ ? 0xffffffffu : 0u);
    size_offset_ = header.size() - (wave64_ ? 8u : 4u);
    this->output.Write(header);
  }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    if (bytes.size() % alignment_) Fail(ContainerErrorCode::InvalidArgument, "WAVE packet splits a sample frame");
    const auto frames = static_cast<std::int64_t>(bytes.size() / alignment_);
    const auto rate = *tracks[0].descriptions.front().sample_rate;
    const MediaTimestamp time{frames_, rate}, length{frames, rate};
    if ((packet.pts && *packet.pts != time) || (packet.dts && *packet.dts != time) ||
        (packet.duration && *packet.duration != length))
      Fail(ContainerErrorCode::InvalidArgument, "WAVE packet timing does not match its contiguous sample grid");
    const auto end = AddOffset(output.Position(), bytes.size());
    if (!rf64_ && !wave64_ && AddOffset(end, end & 1) - 8 > 0xffffffffu)
      Fail(ContainerErrorCode::LimitExceeded, "WAVE file exceeds RIFF size limits; use RF64 or Wave64");
    const auto next = AddTime(frames_, frames);
    output.Write(bytes);
    frames_ = next;
    data_bytes_ += bytes.size();
    return {};
  }

  void Finish() override {
    constexpr std::array<std::byte, 7> padding{};
    const auto count = wave64_ ? Wave64Padding(data_bytes_) : data_bytes_ & 1;
    if (count) output.Write(ByteView(padding).first(static_cast<std::size_t>(count)));
    // The RF64 table carries real lengths; the RIFF and data size fields keep their sentinels.
    if (rf64_) {
      Bytes sizes;
      Put(sizes, output.Position() - 8, true);
      Put(sizes, data_bytes_, true);
      Put(sizes, frames_, true);
      output.Patch(20, sizes);
    } else if (wave64_) {
      Patch(16, output.Position());
      Patch(size_offset_, AddOffset(data_bytes_, Wave64HeaderBytes));
      Patch(fact_offset_, static_cast<std::uint64_t>(frames_));
    } else {
      Patch(4, static_cast<std::uint32_t>(output.Position() - 8));
      Patch(size_offset_, static_cast<std::uint32_t>(data_bytes_));
    }
    if (fact_offset_ && !wave64_) Patch(fact_offset_, static_cast<std::uint32_t>(std::min<std::int64_t>(frames_, 0xffffffffu)));
  }

private:
  void PutChunk(Bytes& header, std::uint32_t type, std::uint32_t size) const {
    if (wave64_) {
      PutWave64Guid(header, type);
      Put(header, Wave64HeaderBytes + size, true);
    } else {
      Put(header, type);
      Put(header, size, true);
    }
  }

  template<class T> void Patch(std::uint64_t offset, T value) {
    Bytes bytes;
    Put(bytes, value, true);
    output.Patch(offset, bytes);
  }

  bool rf64_, wave64_;
  unsigned alignment_;
  std::uint64_t size_offset_;
  std::uint64_t fact_offset_ = 0;
  std::uint64_t data_bytes_ = 0;
  std::int64_t frames_ = 0;
};
} // namespace

bool LooksLikeWave64(ByteView prefix) {
  const auto riff = std::as_bytes(std::span(Wave64RiffTail));
  return prefix.size() >= 12 && Number<std::uint32_t>(prefix) == Tag("riff") &&
      std::ranges::equal(prefix.subspan(4, 8), riff.first(8));
}

void ValidateWave(std::span<const MediaTrack> tracks) {
  if (tracks.size() != 1 || tracks[0].kind != TrackKind::Audio ||
      tracks[0].descriptions.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "WAVE requires one interleaved PCM track");
  const auto& track = tracks[0];
  const auto& description = track.descriptions.front();
  const auto bits = Bits(description.codec);
  const auto channels = description.channel_count.value_or(0), rate = description.sample_rate.value_or(0);
  const auto alignment = static_cast<std::uint64_t>(channels) * (bits / 8);
  if (!channels || !rate || alignment > 65535 || alignment * rate > 0xffffffffu ||
      (track.timescale && track.timescale != rate))
    Fail(ContainerErrorCode::InvalidArgument, "Invalid WAVE sample rate, clock, or channel count");
  if (track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(ContainerErrorCode::Unsupported, "WAVE cannot preserve compressed audio trim");
  if (const auto mask = description.speaker_mask; mask && *mask && std::popcount(*mask) != static_cast<int>(channels))
    Fail(ContainerErrorCode::InvalidArgument, "WAVE speaker mask does not match its channel count");
  if (!description.initialization_data.empty() &&
      (PcmCodec(ExtensibleFormat(description.initialization_data, bits, channels), bits) != description.codec ||
       (description.speaker_mask && *description.speaker_mask != Number<std::uint32_t>(description.initialization_data, 2, true))))
    Fail(ContainerErrorCode::InvalidArgument, "WAVE extensible configuration does not match its coding or speaker mask");
}

std::unique_ptr<ContainerReader> OpenWaveReader(ContainerInput input, ContainerFormat format) {
  return std::make_unique<WaveReader>(std::move(input), format);
}

std::unique_ptr<ContainerWriter> OpenWaveWriter(ContainerOutput output, ContainerFormat format,
                                                std::span<const MediaTrack> tracks) {
  return std::make_unique<WaveWriter>(std::move(output), format, tracks);
}

} // namespace huxerui::media::detail
