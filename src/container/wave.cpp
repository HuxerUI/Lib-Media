#include "container_io.h"

#include "codec/common.h"

#include <array>

namespace huxerui::media::detail {
namespace {

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
    track.initialization_data.assign(suffix.begin(), suffix.end());
  }
  track.codec = PcmCodec(format, bits);
  if (!channels || !rate || alignment != channels * (bits / 8) ||
      byte_rate != static_cast<std::uint64_t>(alignment) * rate)
    Fail(ContainerErrorCode::InvalidData, "Inconsistent WAVE sample rate, byte rate, or frame alignment");
  track.id = 1;
  track.kind = TrackKind::Audio;
  track.sample_rate = track.timescale = rate;
  track.channel_count = channels;
}

class WaveReader final : public ContainerReader {
public:
  WaveReader(ContainerInput input, ContainerFormat format) : ContainerReader(std::move(input), format) {
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

    MediaTrack track;
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
        if (alignment_ || size < 16 || size > 65536) Fail(ContainerErrorCode::InvalidData, "Invalid WAVE format chunk");
        ReadFormat(this->input.ReadBytes(static_cast<std::size_t>(size)), track, alignment_);
        this->input.Skip(size & 1);
      } else if (type == Tag("data")) {
        if (!alignment_ || size % alignment_ || size / alignment_ > std::numeric_limits<std::int64_t>::max())
          Fail(ContainerErrorCode::InvalidData, "WAVE data is missing a format or ends inside a sample frame");
        offset_ = this->input.Position();
        frames_ = static_cast<std::int64_t>(size / alignment_);
        odd_ = size & 1;
        start = MediaTimestamp{0, *track.sample_rate};
        duration = MediaTimestamp{frames_, *track.sample_rate};
        tracks.push_back(std::move(track));
        return;
      } else {
        if ((!this->input.Random() || !this->input.Size()) && padded > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "WAVE header skip exceeds its budget");
        this->input.Skip(padded);
      }
    }
    Fail(ContainerErrorCode::InvalidData, "Missing WAVE data chunk");
  }

  std::optional<DemuxItem> Read() override {
    if (cursor_ == frames_) {
      if (odd_) {
        input.Skip(1);
        odd_ = false;
      }
      return {};
    }
    // Emit bounded blocks without ever splitting an interleaved sample frame.
    const auto frames = std::min<std::int64_t>(frames_ - cursor_, std::max(1u, 16384 / alignment_));
    const auto size = static_cast<std::size_t>(frames) * alignment_;
    auto bytes = Own(input.ReadBytes(size));
    const auto time = MediaTimestamp{cursor_, *tracks[0].sample_rate};
    cursor_ += frames;
    return ContainerPacket{1, std::move(bytes), time, time, MediaTimestamp{frames, time.timescale}, true};
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override {
    if (seek && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "WAVE input cannot seek");
    if (!frames_) return {};
    const auto rate = *tracks[0].sample_rate;
    const MediaTimestamp first{0, rate}, last{frames_ - 1, rate};
    std::int64_t frame = 0;
    if (time.Compare(first) < 0) {
      if (direction == SeekDirection::AtOrBefore) return {};
    } else if (time.Compare(last) > 0) {
      if (direction == SeekDirection::AtOrAfter) return {};
      frame = last.ticks;
    } else {
      frame = time.Rescale(rate)->ticks;
      const auto comparison = MediaTimestamp{frame, rate}.Compare(time);
      if (direction == SeekDirection::AtOrBefore && comparison > 0) --frame;
      if (direction == SeekDirection::AtOrAfter && comparison < 0) ++frame;
    }
    const auto offset = AddOffset(offset_, static_cast<std::uint64_t>(frame) * alignment_);
    if (seek) {
      input.Seek(offset);
      cursor_ = frame;
    }
    return SeekPoint{id, {frame, rate}, SeekPoint::Kind::Independent, offset, {}};
  }

private:
  unsigned alignment_ = 0;
  std::uint64_t offset_ = 0;
  std::int64_t frames_ = 0;
  std::int64_t cursor_ = 0;
  bool odd_ = false;
};

class WaveWriter final : public ContainerWriter {
public:
  WaveWriter(ContainerOutput output, ContainerFormat format, std::span<const MediaTrack> tracks)
      : ContainerWriter(std::move(output), tracks), rf64_(format == ContainerFormat::Rf64) {
    const auto& track = tracks[0];
    const auto bits = Bits(track.codec);
    const auto channels = *track.channel_count;
    const auto rate = *track.sample_rate;
    alignment_ = channels * (bits / 8);
    const std::uint16_t coding = track.codec == CodecId::PcmF32Le ? 3 : 1;
    const bool extensible = channels > 2 || !track.initialization_data.empty();
    Bytes header;
    Put(header, Tag(rf64_ ? "RF64" : "RIFF"));
    Put(header, rf64_ ? 0xffffffffu : 0u, true);
    Put(header, Tag("WAVE"));
    if (rf64_) {
      Put(header, Tag("ds64"));
      Put(header, 28u, true);
      header.resize(header.size() + 28);
    }
    Put(header, Tag("fmt "));
    Put(header, extensible ? 40u : (coding == 3 ? 18u : 16u), true);
    Put<std::uint16_t>(header, extensible ? 0xfffe : coding, true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(channels), true);
    Put(header, rate, true);
    Put(header, rate * alignment_, true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(alignment_), true);
    Put<std::uint16_t>(header, static_cast<std::uint16_t>(bits), true);
    if (extensible) {
      Put<std::uint16_t>(header, 22, true);
      if (track.initialization_data.empty()) {
        Put<std::uint16_t>(header, static_cast<std::uint16_t>(bits), true);
        Put(header, 0u, true);
        Put<std::uint32_t>(header, coding, true);
        Append(header, WaveGuidTail);
      } else {
        Append(header, track.initialization_data);
      }
    } else if (coding == 3) {
      Put<std::uint16_t>(header, 0, true);
    }
    if (coding == 3) {
      Put(header, Tag("fact"));
      Put(header, 4u, true);
      fact_offset_ = header.size();
      Put(header, 0u, true);
    }
    Put(header, Tag("data"));
    size_offset_ = header.size();
    Put(header, rf64_ ? 0xffffffffu : 0u, true);
    this->output.Write(header);
  }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    if (bytes.size() % alignment_) Fail(ContainerErrorCode::InvalidArgument, "WAVE packet splits a sample frame");
    const auto frames = static_cast<std::int64_t>(bytes.size() / alignment_);
    const auto rate = *tracks[0].sample_rate;
    const MediaTimestamp time{frames_, rate}, length{frames, rate};
    if ((packet.pts && *packet.pts != time) || (packet.dts && *packet.dts != time) ||
        (packet.duration && *packet.duration != length))
      Fail(ContainerErrorCode::InvalidArgument, "WAVE packet timing does not match its contiguous sample grid");
    const auto end = AddOffset(output.Position(), bytes.size());
    if (!rf64_ && AddOffset(end, end & 1) - 8 > 0xffffffffu)
      Fail(ContainerErrorCode::LimitExceeded, "WAVE file exceeds RIFF size limits; use RF64");
    const auto next = AddTime(frames_, frames);
    output.Write(bytes);
    frames_ = next;
    data_bytes_ += bytes.size();
    return {};
  }

  void Finish() override {
    if (data_bytes_ & 1) {
      constexpr std::array padding{std::byte{0}};
      output.Write(padding);
    }
    // The RF64 table carries real lengths; the RIFF and data size fields keep their sentinels.
    if (rf64_) {
      Bytes sizes;
      Put(sizes, output.Position() - 8, true);
      Put(sizes, data_bytes_, true);
      Put(sizes, frames_, true);
      output.Patch(20, sizes);
    } else {
      Patch32(4, static_cast<std::uint32_t>(output.Position() - 8));
      Patch32(size_offset_, static_cast<std::uint32_t>(data_bytes_));
    }
    if (fact_offset_) Patch32(fact_offset_, static_cast<std::uint32_t>(std::min<std::int64_t>(frames_, 0xffffffffu)));
  }

private:
  void Patch32(std::uint64_t offset, std::uint32_t value) {
    Bytes bytes;
    Put(bytes, value, true);
    output.Patch(offset, bytes);
  }

  bool rf64_;
  unsigned alignment_;
  std::uint64_t size_offset_;
  std::uint64_t fact_offset_ = 0;
  std::uint64_t data_bytes_ = 0;
  std::int64_t frames_ = 0;
};
} // namespace

void ValidateWave(std::span<const MediaTrack> tracks) {
  if (tracks.size() != 1 || tracks[0].kind != TrackKind::Audio ||
      tracks[0].framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "WAVE requires one interleaved PCM track");
  const auto& track = tracks[0];
  const auto bits = Bits(track.codec);
  const auto channels = track.channel_count.value_or(0), rate = track.sample_rate.value_or(0);
  const auto alignment = static_cast<std::uint64_t>(channels) * (bits / 8);
  if (!channels || !rate || alignment > 65535 || alignment * rate > 0xffffffffu ||
      (track.timescale && track.timescale != rate))
    Fail(ContainerErrorCode::InvalidArgument, "Invalid WAVE sample rate, clock, or channel count");
  if (track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(ContainerErrorCode::Unsupported, "WAVE cannot preserve compressed audio trim");
  if (!track.initialization_data.empty() &&
      PcmCodec(ExtensibleFormat(track.initialization_data, bits, channels), bits) != track.codec)
    Fail(ContainerErrorCode::InvalidArgument, "WAVE extensible configuration does not match its coding");
}

std::unique_ptr<ContainerReader> OpenWaveReader(ContainerInput input, ContainerFormat format) {
  return std::make_unique<WaveReader>(std::move(input), format);
}

std::unique_ptr<ContainerWriter> OpenWaveWriter(ContainerOutput output, ContainerFormat format,
                                                std::span<const MediaTrack> tracks) {
  return std::make_unique<WaveWriter>(std::move(output), format, tracks);
}

} // namespace huxerui::media::detail
