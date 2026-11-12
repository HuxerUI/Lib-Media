#include "framed_audio.h"
#include "codec/common.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>

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
  std::int64_t Signed(unsigned width) {
    if (!width) return 0;
    const auto value = Get(width);
    const auto sign = std::int64_t{1} << (width - 1);
    return (value ^ sign) - sign;
  }
  void Skip(std::size_t count) {
    Require(count);
    position_ += count;
  }
  std::uint64_t Unary() {
    const auto start = position_;
    while (!Get(1)) {}
    return position_ - start - 1;
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

void Residual(FrameBits& bits, unsigned block, unsigned warmup, std::span<std::int64_t> samples) {
  const auto method = bits.Get(2);
  if (method > 1) Fail(Bad, "Reserved FLAC residual method");
  const auto partitions = 1u << bits.Get(4);
  const auto per_partition = block / partitions;
  if (block % partitions || per_partition < warmup) Fail(Bad, "Invalid FLAC residual partition size");
  const auto rice_width = 4 + method;
  const auto escape = (1u << rice_width) - 1;
  auto out = samples.empty() ? samples : samples.subspan(warmup);
  for (unsigned p = 0; p < partitions; ++p) {
    const auto parameter = bits.Get(rice_width);
    const auto count = per_partition - (p == 0 ? warmup : 0);
    if (parameter == escape) {
      const auto width = bits.Get(5);
      if (out.empty()) bits.Skip(std::size_t(count) * width);
      else for (auto& sample : out.first(count)) sample = bits.Signed(width);
    } else {
      for (unsigned i = 0; i < count; ++i) {
        const auto quotient = bits.Unary();
        if (out.empty()) {
          bits.Skip(parameter);
          continue;
        }
        if (quotient > (UINT32_MAX >> parameter)) Fail(Bad, "FLAC Rice residual exceeds its signed range");
        const auto folded = (quotient << parameter) | bits.Get(parameter);
        if (folded == UINT32_MAX) Fail(Bad, "FLAC Rice residual exceeds its signed range");
        const auto half = static_cast<std::int64_t>(folded >> 1);
        out[i] = folded & 1 ? -half - 1 : half;
      }
    }
    if (!out.empty()) out = out.subspan(count);
  }
}

void Subframe(FrameBits& bits, unsigned block, unsigned width, std::span<std::int64_t> samples = {}) {
  if (bits.Get(1)) Fail(Bad, "Invalid FLAC subframe padding");
  const auto type = bits.Get(6);
  const auto wasted = bits.Get(1) ? bits.Wasted(width) : 0u;
  width -= wasted;
  if (type <= 1) {
    if (samples.empty()) bits.Skip(std::size_t(type ? block : 1) * width);
    else if (type) for (auto& sample : samples) sample = bits.Signed(width);
    else std::ranges::fill(samples, bits.Signed(width));
  } else {
    const bool lpc = type >= 32;
    if (!lpc && (type < 8 || type > 12)) Fail(Bad, "Reserved FLAC subframe type");
    const auto order = lpc ? type - 31 : type - 8;
    if (order > block) Fail(Bad, "FLAC predictor exceeds the block size");
    if (samples.empty()) bits.Skip(std::size_t(order) * width);
    else for (auto& sample : samples.first(order)) sample = bits.Signed(width);
    std::array<int, 32> coefficients{};
    unsigned shift = 0;
    if (lpc) {
      const auto precision = bits.Get(4) + 1;
      if (precision == 16) Fail(Bad, "Reserved FLAC LPC precision");
      shift = bits.Get(5);
      if (shift > 15) Fail(Bad, "Negative FLAC LPC shift");
      if (samples.empty()) bits.Skip(std::size_t(order) * precision);
      else for (auto& value : std::span(coefficients).first(order)) value = static_cast<int>(bits.Signed(precision));
    } else if (!samples.empty()) {
      constexpr int Fixed[5][4]{{}, {1}, {2, -1}, {3, -3, 1}, {4, -6, 4, -1}};
      std::copy_n(Fixed[order], order, coefficients.begin());
    }
    Residual(bits, block, order, samples);
    const auto bound = std::int64_t{1} << (width - 1);
    for (std::size_t i = order; i < samples.size(); ++i) {
      std::int64_t prediction = 0;
      for (unsigned j = 0; j < order; ++j) prediction += coefficients[j] * samples[i - j - 1];
      samples[i] += prediction >> shift;
      if (samples[i] < -bound || samples[i] >= bound) Fail(Bad, "FLAC predictor exceeds the coded sample width");
    }
  }
  if (wasted)
    for (auto& sample : samples) sample *= std::int64_t{1} << wasted;
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

void ReadFrameChecksum(FrameBits& bits) {
  bits.Align();
  const auto checksum = bits.Get(16);
  if (Crc(bits.BytesRead().first(bits.BytesUsed() - 2), 0x8005, 16) != checksum)
    Fail(Bad, "FLAC frame checksum differs");
}

unsigned ValidateFrame(FrameBits& bits, const FlacStreamInfo& info, std::int64_t position) {
  const auto [block, channels] = ReadFrameHeader(bits, info, position);
  for (unsigned channel = 0; channel < info.channels; ++channel) {
    const bool side = (channels == 8 && channel == 1) || (channels == 9 && channel == 0) ||
                      (channels == 10 && channel == 1);
    Subframe(bits, block, info.bits + side);
  }
  ReadFrameChecksum(bits);
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

constexpr std::string_view SpeakerMaskTag = "WAVEFORMATEXTENSIBLE_CHANNEL_MASK";

std::uint32_t DefaultFlacSpeakerMask(unsigned channels) {
  constexpr std::array masks{0x4u, 0x3u, 0x7u, 0x33u, 0x37u, 0x3fu, 0x70fu, 0x63fu};
  return masks[channels - 1];
}

void ReadComments(ByteView bytes, MediaAudioMetadata& metadata, std::optional<std::uint32_t>& speaker_mask) {
  SizedField(bytes, true);
  const auto count = Number<std::uint32_t>(bytes, 0, true);
  bytes = bytes.subspan(4);
  if (count > bytes.size() / 4) Fail(Bad, "Invalid FLAC comment count");
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto comment = Text(SizedField(bytes, true));
    const auto separator = comment.find('=');
    if (separator == std::string_view::npos || separator > SpeakerMaskTag.size()) continue;
    std::string key(comment.substr(0, separator));
    for (auto& c : key) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const auto value = comment.substr(separator + 1);
    if (key == SpeakerMaskTag) {
      std::uint32_t mask = 0;
      if (speaker_mask || value.size() < 3 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X'))
        Fail(Bad, "Invalid FLAC speaker mask comment");
      const auto result = std::from_chars(value.data() + 2, value.data() + value.size(), mask, 16);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        Fail(Bad, "Invalid FLAC speaker mask value");
      speaker_mask = mask;
      continue;
    }
    const auto tag = std::ranges::find(AudioTags, key, &AudioTag::flac);
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
    std::optional<std::uint32_t> speaker_mask;
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
        else if (type == 4) ReadComments(this->input.ReadBytes(size), tags, speaker_mask);
        else if (type == 6) ReadPicture(this->input.ReadBytes(size), tags);
        else this->input.Skip(size);
      }
      metadata += size + 4;
    }
    if (speaker_mask && std::popcount(*speaker_mask) > static_cast<int>(info_.channels))
      Fail(Bad, "FLAC speaker mask exceeds its channel count");
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .timescale = info_.rate,
        .descriptions = {{.codec = CodecId::Flac, .initialization_data = std::move(stream_info),
                          .sample_rate = info_.rate, .channel_count = info_.channels,
                          .speaker_mask = speaker_mask.value_or(DefaultFlacSpeakerMask(info_.channels))}}};
    if (HasAudioMetadata(tags)) audio_metadata = std::move(tags);
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
          point = SeekPoint{id, time, SeekPoint::Kind::Independent, offset, time};
        if (order >= 0) break;
        sample = AddTime(sample, frame->samples);
      }
    } catch (const ContainerError& error) {
      input.Seek(previous);
      if (error.code != Bad) throw;
      seek_table_.clear();
      return FramedAudioReader::Find(id, target, direction, seek);
    }
    if (seek && point) SeekFrame(*point->byte_offset, point->decode_start_pts.ticks);
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

void WriteTags(ContainerOutput& output, const MediaAudioMetadata& metadata, std::optional<std::uint32_t> mask) {
  const auto fields = std::ranges::count_if(AudioTags, [&](const auto& tag) { return !(metadata.*tag.field).empty(); });
  if (fields || mask) {
    Bytes comments;
    constexpr std::string_view vendor = "Lib-Media";
    Put(comments, static_cast<std::uint32_t>(vendor.size()), true);
    Append(comments, std::as_bytes(std::span(vendor)));
    Put(comments, static_cast<std::uint32_t>(fields + mask.has_value()), true);
    for (const auto& tag : AudioTags) {
      const auto& value = metadata.*tag.field;
      if (value.empty()) continue;
      Put(comments, static_cast<std::uint32_t>(tag.flac.size() + 1 + value.size()), true);
      Append(comments, std::as_bytes(std::span(tag.flac)));
      comments.push_back(std::byte{'='});
      Append(comments, std::as_bytes(std::span(value)));
    }
    if (mask) {
      std::array<char, 8> hex;
      const auto value = std::to_chars(hex.data(), hex.data() + hex.size(), *mask, 16);
      Put(comments, static_cast<std::uint32_t>(SpeakerMaskTag.size() + 3 + (value.ptr - hex.data())), true);
      Append(comments, std::as_bytes(std::span(SpeakerMaskTag)));
      constexpr std::string_view prefix = "=0x";
      Append(comments, std::as_bytes(std::span(prefix)));
      Append(comments, std::as_bytes(std::span(hex.data(), value.ptr)));
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
  FlacWriter(ContainerOutput output, std::span<const MediaTrack> tracks,
             const std::optional<MediaAudioMetadata>& tags)
      : ContainerWriter(std::move(output), tracks),
        info_(ParseFlacStreamInfo(tracks.front().descriptions.front().initialization_data, ContainerErrorCode::InvalidArgument)),
        seek_interval_(std::max<std::uint64_t>(info_.rate, (info_.total + SeekSlots - 1) / SeekSlots)) {
    Bytes signature;
    Put(signature, Tag("fLaC"));
    this->output.Write(signature);
    WriteMetadata(this->output, 0, tracks.front().descriptions.front().initialization_data);
    auto mask = tracks.front().descriptions.front().speaker_mask;
    if (mask == DefaultFlacSpeakerMask(info_.channels)) mask.reset();
    if (tags || mask) {
      const MediaAudioMetadata empty;
      WriteTags(this->output, tags ? *tags : empty, mask);
    }
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
    auto info = tracks.front().descriptions.front().initialization_data;
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

AudioFrame DecodeMultichannelFlac(ByteView bytes, const FlacStreamInfo& info, AudioFormat format, MediaTimestamp pts) {
  FrameBits bits(bytes);
  const auto block = ReadFrameHeader(bits, info).block;
  format.sample_format = AudioSampleFormat::F32;
  format.packing = AudioPacking::Planar;
  AudioFrame frame{format, pts, block, {}};
  std::vector<std::int64_t> samples(block);
  const auto scale = std::ldexp(1.0f, 1 - static_cast<int>(info.bits));
  for (unsigned channel = 0; channel < info.channels; ++channel) {
    Subframe(bits, block, info.bits, samples);
    Bytes plane(std::size_t(block) * sizeof(float));
    for (unsigned i = 0; i < block; ++i) {
      const float sample = static_cast<float>(samples[i]) * scale;
      std::memcpy(plane.data() + std::size_t(i) * sizeof(sample), &sample, sizeof(sample));
    }
    frame.planes.push_back(Own(std::move(plane)));
  }
  ReadFrameChecksum(bits);
  if (bits.BytesUsed() != bytes.size()) Fail(Bad, "Trailing bytes after the FLAC frame");
  return frame;
}

std::unique_ptr<ContainerReader> OpenFlacReader(ContainerInput input) {
  return std::make_unique<FlacReader>(std::move(input));
}

void ValidateFlac(std::span<const MediaTrack> tracks) {
  constexpr auto argument = ContainerErrorCode::InvalidArgument;
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio || tracks.front().descriptions.front().codec != CodecId::Flac ||
      tracks.front().descriptions.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "Native FLAC output requires one FLAC track");
  const auto& track = tracks.front();
  const auto& description = track.descriptions.front();
  const auto info = ParseFlacStreamInfo(description.initialization_data, argument);
  if (description.sample_rate != info.rate || description.channel_count != info.channels ||
      track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(argument, "FLAC track geometry or trim differs from STREAMINFO");
  if (description.speaker_mask && std::popcount(*description.speaker_mask) > static_cast<int>(info.channels))
    Fail(argument, "FLAC speaker mask exceeds its channel count");
}

std::unique_ptr<ContainerWriter> OpenFlacWriter(ContainerOutput output, std::span<const MediaTrack> tracks,
                                               const std::optional<MediaAudioMetadata>& tags) {
  return std::make_unique<FlacWriter>(std::move(output), tracks, tags);
}
} // namespace huxerui::media::detail
