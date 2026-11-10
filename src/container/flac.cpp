#include "framed_audio.h"

#include <array>

namespace huxerui::media::detail {

FlacStreamInfo ParseFlacStreamInfo(ByteView bytes, ContainerErrorCode code) {
  if (bytes.size() != 34) Fail(code, "FLAC STREAMINFO must be 34 bytes");
  const auto packed = Number<std::uint64_t>(bytes, 10);
  const auto result = FlacStreamInfo{Number<std::uint16_t>(bytes), Number<std::uint16_t>(bytes, 2),
      Number<std::uint32_t>(bytes, 6) & 0xffffffu, unsigned(packed >> 44), unsigned((packed >> 41) & 7) + 1,
      unsigned((packed >> 36) & 31) + 1, packed & ((std::uint64_t{1} << 36) - 1)};
  if (result.min_block < 16 || result.max_block < result.min_block || !result.rate || result.rate > 655350 ||
      result.bits < 4) Fail(code, "Invalid FLAC STREAMINFO geometry");
  return result;
}

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

class FrameBits {
public:
  FrameBits(ContainerInput& input, std::size_t limit) : input_(&input), limit_(limit) {}
  explicit FrameBits(ByteView bytes) : limit_(bytes.size()), bytes_(bytes) {}
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
      Fail(input_ ? ContainerErrorCode::LimitExceeded : Bad, "FLAC frame exceeds its size limit");
    const auto needed = (position_ + count + 7) / 8;
    if (needed <= bytes_.size()) return;
    bytes_ = input_->Peek(std::min(limit_, (needed + 4095) / 4096 * 4096));
    if (needed > bytes_.size()) Fail(Bad, "Truncated FLAC frame");
  }
  ContainerInput* input_ = nullptr;
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

struct FlacFrameHeader {
  unsigned block, channels;
};

FlacFrameHeader ReadFrameHeader(FrameBits& bits, const FlacStreamInfo& info,
                                std::optional<std::int64_t> position = {}) {
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
      block > info.max_block || (!variable && number > 0x7fffffffu) ||
      (position && (variable ? number : number * info.max_block) != std::uint64_t(*position)))
    Fail(Bad, "FLAC frame does not follow its stream geometry or sample position");
  const auto header_crc = bits.Get(8);
  if (Crc(bits.BytesRead().first(bits.BytesUsed() - 1), 0x07, 8) != header_crc)
    Fail(Bad, "FLAC frame header checksum differs");
  return {block, channels};
}

unsigned ValidateFrame(FrameBits& bits, const FlacStreamInfo& info, std::int64_t position) {
  const auto [block, channels] = ReadFrameHeader(bits, info, position);
  for (unsigned channel = 0; channel < info.channels; ++channel) {
    const bool side = (channels == 8 && channel == 1) || (channels == 9 && channel == 0) ||
                      (channels == 10 && channel == 1);
    Subframe(bits, block, info.bits + side);
  }
  bits.Align();
  const auto checksum = bits.Get(16);
  if (Crc(bits.BytesRead().first(bits.BytesUsed() - 2), 0x8005, 16) != checksum)
    Fail(Bad, "FLAC frame checksum differs");
  return block;
}

std::string_view Text(ByteView bytes) {
  return bytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ByteView SizedField(ByteView& bytes, bool little = false) {
  const auto size = Number<std::uint32_t>(bytes, 0, little);
  const auto value = Slice(bytes, 4, size);
  bytes = bytes.subspan(4 + size);
  return value;
}

void ReadComments(ByteView bytes, MediaAudioMetadata& metadata) {
  SizedField(bytes, true);
  const auto count = Number<std::uint32_t>(bytes, 0, true);
  bytes = bytes.subspan(4);
  if (count > bytes.size() / 4) Fail(Bad, "Invalid FLAC comment count");
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto comment = Text(SizedField(bytes, true));
    const auto separator = comment.find('=');
    if (separator == std::string_view::npos || separator > 11) continue;
    std::string key(comment.substr(0, separator));
    for (auto& c : key) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const auto tag = std::ranges::find(AudioTags, key, &AudioTag::flac);
    const auto value = comment.substr(separator + 1);
    if (tag != AudioTags.end() && (metadata.*tag->field).empty() && value.size() <= MaxAudioText &&
        value.find('\0') == std::string_view::npos) metadata.*tag->field = value;
  }
}

void ReadPicture(ByteView bytes, MediaAudioMetadata& metadata) {
  if (metadata.cover || Number<std::uint32_t>(bytes) != 3) return;
  bytes = bytes.subspan(4);
  const auto mime = Text(SizedField(bytes));
  SizedField(bytes);
  if (bytes.size() < 16) Fail(Bad, "Truncated FLAC picture properties");
  bytes = bytes.subspan(16);
  const auto data = SizedField(bytes);
  if (!bytes.empty()) Fail(Bad, "FLAC picture has trailing data");
  if (IsCoverMime(mime) && !data.empty() && data.size() <= MaxContainerPacket - 64) {
    auto owner = std::make_shared<const Bytes>(data.begin(), data.end());
    metadata.cover = MediaAudioMetadata::Cover{std::string(mime), BufferReference(*owner, owner)};
  }
}

struct FlacSeekEntry {
  std::int64_t sample;
  std::uint64_t offset;
  unsigned samples;
};

std::vector<FlacSeekEntry> ReadSeekTable(ByteView bytes, const FlacStreamInfo& info) {
  if (bytes.size() % 18 || bytes.size() / 18 > MaxContainerSamples) return {};
  std::vector<FlacSeekEntry> entries;
  while (!bytes.empty()) {
    const auto sample = Number<std::uint64_t>(bytes);
    if (sample == UINT64_MAX) break;
    const auto offset = Number<std::uint64_t>(bytes, 8);
    const auto samples = Number<std::uint16_t>(bytes, 16);
    if (sample > INT64_MAX || (info.total && sample >= info.total) || !samples || samples > info.max_block ||
        (!entries.empty() && (sample <= std::uint64_t(entries.back().sample) || offset <= entries.back().offset))) return {};
    entries.push_back({static_cast<std::int64_t>(sample), offset, samples});
    bytes = bytes.subspan(18);
  }
  return entries;
}

class FlacReader final : public FramedAudioReader {
public:
  explicit FlacReader(ContainerInput input) : FramedAudioReader(std::move(input), ContainerFormat::Flac) {
    this->input.Skip(4);
    Bytes stream_info;
    MediaAudioMetadata tags;
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
        info_ = ParseFlacStreamInfo(stream_info, Bad);
      } else {
        if (!type || type == 127) Fail(Bad, "Invalid FLAC metadata block type");
        if (type == 3 && this->input.Random()) seek_table_ = ReadSeekTable(this->input.ReadBytes(size), info_);
        else if (type == 4) ReadComments(this->input.ReadBytes(size), tags);
        else if (type == 6) ReadPicture(this->input.ReadBytes(size), tags);
        else this->input.Skip(size);
      }
      metadata += size + 4;
    }
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .codec = CodecId::Flac,
                     .initialization_data = std::move(stream_info), .timescale = info_.rate,
                     .sample_rate = info_.rate, .channel_count = info_.channels};
    if (HasAudioMetadata(tags)) track.audio_metadata = std::move(tags);
    Begin(std::move(track), info_.total ? std::optional{info_.total} : std::nullopt);
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp target, SeekDirection direction, bool seek) override {
    if (seek_table_.empty()) return FramedAudioReader::Find(id, target, direction, seek);
    const auto hint = std::upper_bound(seek_table_.begin(), seek_table_.end(), target, [&](MediaTimestamp time, const auto& entry) {
      return time.Compare({entry.sample, info_.rate}) < 0;
    });
    const auto entry = hint == seek_table_.begin() ? FlacSeekEntry{} : *std::prev(hint);
    const auto previous = input.Position();
    std::optional<SeekPoint> point;
    try {
      input.Seek(AddOffset(FirstFrameOffset(), entry.offset));
      auto sample = entry.sample;
      for (std::size_t count = 0;; ++count) {
        if (count == MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "FLAC seek scan exceeds its frame limit");
        const auto offset = input.Position();
        const auto frame = ReadFrame(sample);
        if (!count && entry.samples && (!frame || frame->samples != entry.samples))
          Fail(Bad, "FLAC seek table frame size differs");
        if (!frame) break;
        const MediaTimestamp time{sample, info_.rate};
        const auto order = *time.Compare(target);
        if (direction == SeekDirection::AtOrBefore && order > 0) break;
        if (direction == SeekDirection::AtOrBefore || order >= 0)
          point = SeekPoint{id, time, SeekPoint::Kind::Independent, offset, {}};
        if (order >= 0) break;
        sample = AddTime(sample, frame->samples);
      }
    } catch (const ContainerError& error) {
      input.Seek(previous);
      if (error.code != Bad) throw;
      seek_table_.clear();
      return FramedAudioReader::Find(id, target, direction, seek);
    }
    if (seek && point) SeekFrame(*point->byte_offset, point->time.ticks);
    else input.Seek(previous);
    return point;
  }

private:
  std::optional<AudioPacket> ReadFrame(std::int64_t position) override {
    if (input.Peek(1).empty()) return {};
    FrameBits bits(input, info_.max_frame ? info_.max_frame : MaxContainerPacket);
    const auto samples = ValidateFrame(bits, info_, position);
    return AudioPacket{input.ReadBytes(bits.BytesUsed()), samples};
  }
  FlacStreamInfo info_{};
  std::vector<FlacSeekEntry> seek_table_;
};

void WriteMetadata(ContainerOutput& output, unsigned type, ByteView bytes) {
  Bytes header;
  Put(header, (type << 24) | static_cast<unsigned>(bytes.size()));
  output.Write(header);
  output.Write(bytes);
}

void WriteTags(ContainerOutput& output, const MediaAudioMetadata& metadata) {
  const auto fields = std::ranges::count_if(AudioTags, [&](const auto& tag) { return !(metadata.*tag.field).empty(); });
  if (fields) {
    Bytes comments;
    constexpr std::string_view vendor = "Lib-Media";
    Put(comments, static_cast<std::uint32_t>(vendor.size()), true);
    Append(comments, std::as_bytes(std::span(vendor)));
    Put(comments, static_cast<std::uint32_t>(fields), true);
    for (const auto& tag : AudioTags) {
      const auto& value = metadata.*tag.field;
      if (value.empty()) continue;
      Put(comments, static_cast<std::uint32_t>(tag.flac.size() + 1 + value.size()), true);
      Append(comments, std::as_bytes(std::span(tag.flac)));
      comments.push_back(std::byte{'='});
      Append(comments, std::as_bytes(std::span(value)));
    }
    WriteMetadata(output, 4, comments);
  }
  if (metadata.cover) {
    const auto& cover = *metadata.cover;
    Bytes picture;
    Put(picture, 3u);
    Put(picture, static_cast<std::uint32_t>(cover.mime_type.size()));
    Append(picture, std::as_bytes(std::span(cover.mime_type)));
    picture.resize(picture.size() + 20);
    Put(picture, static_cast<std::uint32_t>(cover.data.AsBytes().size()));
    Append(picture, cover.data.AsBytes());
    WriteMetadata(output, 6, picture);
  }
}

class FlacWriter final : public ContainerWriter {
public:
  FlacWriter(ContainerOutput output, std::span<const MediaTrack> descriptions)
      : ContainerWriter(std::move(output), descriptions),
        info_(ParseFlacStreamInfo(tracks.front().initialization_data, ContainerErrorCode::InvalidArgument)),
        seek_interval_(std::max<std::uint64_t>(info_.rate, (info_.total + SeekSlots - 1) / SeekSlots)) {
    Bytes signature;
    Put(signature, Tag("fLaC"));
    this->output.Write(signature);
    WriteMetadata(this->output, 0, tracks.front().initialization_data);
    if (const auto& tags = tracks.front().audio_metadata) WriteTags(this->output, *tags);
    seek_offset_ = this->output.Position() + 4;
    WriteMetadata(this->output, 0x83, Bytes(SeekSlots * 18));
    frames_offset_ = this->output.Position();
  }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    if (bytes.size() >= (1u << 24)) Fail(ContainerErrorCode::LimitExceeded, "FLAC frame exceeds STREAMINFO");
    FrameBits bits(bytes);
    const auto samples = ValidateFrame(bits, info_, sample_);
    if (bits.BytesUsed() != bytes.size())
      Fail(ContainerErrorCode::InvalidArgument, "FLAC output requires one complete frame per packet");
    ValidateAudioPacketTime(packet, sample_, samples, info_.rate);
    if (std::uint64_t(sample_) + samples >= (std::uint64_t{1} << 36))
      Fail(ContainerErrorCode::LimitExceeded, "FLAC sample count exceeds STREAMINFO");
    if (seeks_.size() < SeekSlots && std::uint64_t(sample_) >= next_seek_) {
      seeks_.push_back({sample_, output.Position() - frames_offset_, samples});
      next_seek_ = static_cast<std::uint64_t>(sample_) + seek_interval_;
    }
    const auto size = static_cast<unsigned>(bytes.size());
    minimum_ = std::min(minimum_, size);
    maximum_ = std::max(maximum_, size);
    output.Write(bytes);
    sample_ += samples;
    return {};
  }

  void Finish() override {
    if (!sample_) Fail(ContainerErrorCode::InvalidArgument, "FLAC output needs at least one frame");
    auto info = tracks.front().initialization_data;
    const auto set_size = [&](unsigned offset, unsigned value) {
      for (unsigned i = 0; i < 3; ++i) info[offset + i] = std::byte((value >> (16 - 8 * i)) & 255);
    };
    set_size(4, minimum_);
    set_size(7, maximum_);
    Bytes packed;
    Put(packed, (Number<std::uint64_t>(info, 10) & ~((std::uint64_t{1} << 36) - 1)) | std::uint64_t(sample_));
    std::ranges::copy(packed, info.begin() + 10);
    if (std::uint64_t(sample_) != info_.total) std::fill(info.begin() + 18, info.end(), std::byte{});
    output.Patch(8, info);
    Bytes table;
    for (const auto& entry : seeks_) {
      Put(table, static_cast<std::uint64_t>(entry.sample));
      Put(table, entry.offset);
      Put(table, static_cast<std::uint16_t>(entry.samples));
    }
    for (auto count = seeks_.size(); count < SeekSlots; ++count) {
      Put(table, UINT64_MAX);
      Put(table, std::uint64_t{0});
      Put(table, std::uint16_t{0});
    }
    output.Patch(seek_offset_, table);
  }

private:
  static constexpr unsigned SeekSlots = 128;
  FlacStreamInfo info_;
  std::uint64_t seek_interval_, next_seek_ = 0, seek_offset_ = 0, frames_offset_ = 0;
  std::int64_t sample_ = 0;
  unsigned minimum_ = UINT32_MAX, maximum_ = 0;
  std::vector<FlacSeekEntry> seeks_;
};
} // namespace

unsigned FlacPacketSamples(ByteView bytes, const FlacStreamInfo& info) {
  FrameBits bits(bytes);
  return ReadFrameHeader(bits, info).block;
}

std::unique_ptr<ContainerReader> OpenFlacReader(ContainerInput input) {
  return std::make_unique<FlacReader>(std::move(input));
}

void ValidateFlac(std::span<const MediaTrack> tracks) {
  constexpr auto argument = ContainerErrorCode::InvalidArgument;
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio || tracks.front().codec != CodecId::Flac ||
      tracks.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "Native FLAC output requires one FLAC track");
  const auto& track = tracks.front();
  const auto info = ParseFlacStreamInfo(track.initialization_data, argument);
  if (track.sample_rate != info.rate || track.channel_count != info.channels ||
      track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(argument, "FLAC track geometry or trim differs from STREAMINFO");
  if (track.audio_metadata) ValidateAudioMetadata(*track.audio_metadata);
}

std::unique_ptr<ContainerWriter> OpenFlacWriter(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<FlacWriter>(std::move(output), tracks);
}
} // namespace huxerui::media::detail
