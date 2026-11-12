#include "pcm.h"

#include "codec/common.h"
#include "framed_audio.h"
#include "format/packed_pcm.h"

namespace huxerui::media::detail {
namespace {

constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto WriteError = ContainerErrorCode::InvalidArgument;

struct Chunk {
  std::uint64_t offset = 0, size = 0;
};

struct AudioDescription {
  std::uint32_t rate, format, flags, packet_bytes, packet_frames, channels, bits;
};

AudioDescription ReadDescription(ByteView bytes) {
  const auto rate = std::bit_cast<double>(Number<std::uint64_t>(bytes));
  if (!std::isfinite(rate) || rate <= 0) Fail(Invalid, "Invalid CAF sample rate");
  if (rate > UINT32_MAX || rate != std::trunc(rate))
    Fail(ContainerErrorCode::Unsupported, "CAF sample rate is outside the integer sample clock");
  const auto channels = Number<std::uint32_t>(bytes, 24);
  if (!channels) Fail(Invalid, "CAF channel count is zero");
  if (channels > 2) Fail(ContainerErrorCode::Unsupported, "CAF supports mono and stereo audio");
  return {static_cast<std::uint32_t>(rate), Number<std::uint32_t>(bytes, 8), Number<std::uint32_t>(bytes, 12),
      Number<std::uint32_t>(bytes, 16), Number<std::uint32_t>(bytes, 20), channels, Number<std::uint32_t>(bytes, 28)};
}

ByteView AlacCookie(ByteView cookie, unsigned channels) {
  if (cookie.size() == 24) return cookie;
  if (cookie.size() == 48 && Number<std::uint32_t>(cookie, 24) == 24 &&
      Number<std::uint32_t>(cookie, 28) == Tag("chan") && !Number<std::uint32_t>(cookie, 32)) {
    const auto layout = channels == 1 ? (100u << 16) | 1 : (101u << 16) | 2;
    if (Number<std::uint32_t>(cookie, 36) != layout)
      Fail(ContainerErrorCode::Unsupported, "Unsupported ALAC channel layout in CAF");
    return cookie.first(24);
  }
  if (cookie.size() == 48 && Number<std::uint32_t>(cookie) == 12 &&
      Number<std::uint32_t>(cookie, 4) == Tag("frma") && Number<std::uint32_t>(cookie, 8) == Tag("alac") &&
      Number<std::uint32_t>(cookie, 12) == 36 && Number<std::uint32_t>(cookie, 16) == Tag("alac") &&
      !Number<std::uint32_t>(cookie, 20))
    return cookie.last(24);
  Fail(Invalid, "Invalid CAF ALAC cookie");
}

struct PacketTable {
  std::int64_t count, valid, priming, remainder;

  explicit PacketTable(ByteView bytes) : count(Number<std::int64_t>(bytes)), valid(Number<std::int64_t>(bytes, 8)),
      priming(Number<std::int32_t>(bytes, 16)), remainder(Number<std::int32_t>(bytes, 20)) {
    if (count < 0 || valid < 0 || priming < 0 || remainder < 0)
      Fail(Invalid, "Invalid CAF packet table counts");
  }

  std::int64_t Total() const { return AddTime(AddTime(valid, priming), remainder); }
};

std::uint32_t ReadPacketValue(ByteView& table) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 5; ++i) {
    if (table.empty() || value > UINT32_MAX >> 7) Fail(Invalid, "Invalid CAF packet table integer");
    const auto byte = std::to_integer<unsigned>(table.front());
    table = table.subspan(1);
    value = (value << 7) | (byte & 127);
    if (byte < 128) return value;
  }
  Fail(Invalid, "Overlong CAF packet table integer");
}

void PutPacketValue(Bytes& table, std::uint32_t value) {
  auto shift = (std::bit_width(value | 1u) - 1) / 7 * 7;
  while (shift) {
    Put<std::uint8_t>(table, static_cast<std::uint8_t>(((value >> shift) & 127) | 128));
    shift -= 7;
  }
  Put<std::uint8_t>(table, static_cast<std::uint8_t>(value & 127));
}

void ValidateTrimFields(const MediaTrack& track) {
  if (track.priming_samples.value_or(0) > INT32_MAX || track.padding_samples.value_or(0) > INT32_MAX)
    Fail(ContainerErrorCode::Unsupported, "CAF trim exceeds its packet table fields");
}

std::int64_t CeilSample(MediaTimestamp time, unsigned rate) {
  const auto sample = time.Rescale(rate);
  if (!sample) Fail(WriteError, "CAF edit boundary exceeds its sample clock");
  return sample->Compare(time) < 0 ? AddTime(sample->ticks, 1) : sample->ticks;
}

class CafAlacReader final : public ContainerReader {
public:
  CafAlacReader(ContainerInput input, AudioDescription description, Chunk data, ByteView cookie, ByteView table)
      : ContainerReader(std::move(input), ContainerFormat::Caf), config_(ParseAlac(cookie, Invalid)) {
    if (description.rate != config_.sample_rate || description.channels != config_.channels || description.bits ||
        description.packet_frames > config_.frame_length)
      Fail(Invalid, "CAF ALAC description differs from its cookie");
    const PacketTable header(table);
    auto fields = table.subspan(24);
    auto count = static_cast<std::uint64_t>(header.count);
    if (description.packet_bytes && description.packet_frames) {
      if (data.size % description.packet_bytes) Fail(Invalid, "CAF data splits a fixed-size packet");
      count = data.size / description.packet_bytes;
      if (header.count && static_cast<std::uint64_t>(header.count) != count)
        Fail(Invalid, "CAF packet table count differs from the data size");
    }
    if (count > MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "CAF packet index exceeds its entry limit");
    packets_.reserve(static_cast<std::size_t>(count));
    const auto data_end = AddOffset(data.offset, data.size);
    auto offset = data.offset;
    std::int64_t samples = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto size = description.packet_bytes ? description.packet_bytes : ReadPacketValue(fields);
      const auto frames = description.packet_frames ? description.packet_frames : ReadPacketValue(fields);
      if (!size || !frames || frames > config_.frame_length || size > data_end - offset)
        Fail(Invalid, "CAF packet table entry exceeds its data or sample bounds");
      if (size > MaxContainerPacket) Fail(ContainerErrorCode::LimitExceeded, "CAF ALAC packet exceeds 16 MiB");
      packets_.push_back({offset, samples, size, frames});
      offset += size;
      samples = AddTime(samples, frames);
    }
    if (offset != data_end || header.Total() != samples) Fail(Invalid, "CAF packet table does not cover its data and samples");
    if (!packets_.empty() && description.packet_frames) {
      auto& last = packets_.back();
      this->input.Seek(last.offset);
      const auto actual = AlacPacketSamples(this->input.ReadBytes(last.size), config_, Invalid);
      if (actual > last.frames) Fail(Invalid, "CAF ALAC tail exceeds its nominal packet duration");
      samples = last.start + actual;
      last.frames = actual;
    }
    priming_ = header.priming;
    // Fixed-frame tables can count the uncoded part of a short tail in the remainder.
    audible_end_ = std::min(samples, AddTime(priming_, header.valid));
    if (audible_end_ < priming_ || (!packets_.empty() && audible_end_ == priming_))
      Fail(Invalid, "CAF packet table has no valid audio interval");
    MediaTrack track{.id = 1, .kind = TrackKind::Audio, .timescale = config_.sample_rate,
        .descriptions = {{.codec = CodecId::Alac, .initialization_data = Bytes(cookie.begin(), cookie.end()),
            .sample_rate = config_.sample_rate, .channel_count = config_.channels}}};
    if (priming_) track.priming_samples = static_cast<std::uint64_t>(priming_);
    if (audible_end_ < samples) track.padding_samples = static_cast<std::uint64_t>(samples - audible_end_);
    start = At(0);
    duration = At(audible_end_ - priming_);
    if (track.priming_samples || track.padding_samples) track.edits = {{*duration, At(priming_)}};
    tracks.push_back(std::move(track));
    first_audible_pts[0] = At(priming_);
    this->input.Seek(data.offset);
  }

  std::optional<ContainerPacket> Read() override {
    if (cursor_ == packets_.size()) return {};
    const auto& packet = packets_[cursor_];
    auto bytes = input.ReadBytes(packet.size);
    if (AlacPacketSamples(bytes, config_, Invalid) != packet.frames)
      Fail(Invalid, "CAF packet table disagrees with the ALAC frame header");
    ++cursor_;
    return ContainerPacket{1, Own(std::move(bytes)), At(packet.start), At(packet.start), At(packet.frames), true};
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override {
    const auto first = std::partition_point(packets_.begin(), packets_.end(), [&](const Packet& packet) {
      return packet.start + packet.frames <= priming_;
    });
    const auto end = std::lower_bound(first, packets_.end(), audible_end_, [](const Packet& packet, std::int64_t sample) {
      return packet.start < sample;
    });
    const auto present = [&](const Packet& packet) { return At(std::max<std::int64_t>(0, packet.start - priming_)); };
    auto point = std::lower_bound(first, end, time, [&](const Packet& packet, MediaTimestamp target) {
      return present(packet).Compare(target) < 0;
    });
    if (direction == SeekDirection::AtOrBefore && (point == end || present(*point) != time)) {
      if (point == first) return {};
      --point;
    }
    if (point == end) return {};
    if (seek) {
      input.Seek(point->offset);
      cursor_ = static_cast<std::size_t>(point - packets_.begin());
    }
    return SeekPoint{id, present(*point), SeekPoint::Kind::Independent, point->offset, At(point->start)};
  }

  std::optional<MediaPresentationRange> PresentationRange(std::size_t) const override {
    return MediaPresentationRange{*start, *duration};
  }

private:
  struct Packet { std::uint64_t offset; std::int64_t start; std::uint32_t size, frames; };
  MediaTimestamp At(std::int64_t sample) const { return {sample, config_.sample_rate}; }

  AlacConfig config_;
  std::vector<Packet> packets_;
  std::size_t cursor_ = 0;
  std::int64_t priming_ = 0, audible_end_ = 0;
};

class CafWriter final : public ContainerWriter {
public:
  CafWriter(ContainerOutput output, std::span<const MediaTrack> tracks) : ContainerWriter(std::move(output), tracks) {
    const auto& description = tracks.front().descriptions.front();
    rate_ = *description.sample_rate;
    if (description.codec == CodecId::Alac) alac_ = ParseAlac(description.initialization_data, WriteError);
    const auto pcm = DescribePackedPcm(description.codec);
    alignment_ = *description.channel_count * (pcm.bits / 8);
    const auto alac_flags = alac_ ? (alac_->bits == 32 ? 4u : (alac_->bits - 16) / 4 + 1) : 0u;
    const auto pcm_flags = (pcm.floating_point ? 1u : 0u) | (pcm.big_endian ? 0u : 2u);
    Bytes header;
    Put(header, Tag("caff"));
    Put<std::uint16_t>(header, 1);
    Put<std::uint16_t>(header, 0);
    Put(header, Tag("desc"));
    Put<std::int64_t>(header, 32);
    Put(header, std::bit_cast<std::uint64_t>(double(rate_)));
    Put(header, Tag(alac_ ? "alac" : "lpcm"));
    Put(header, alac_ ? alac_flags : pcm_flags);
    Put(header, alignment_);
    packet_frames_offset_ = header.size();
    Put(header, alac_ ? alac_->frame_length : 1u);
    Put(header, *description.channel_count);
    Put(header, pcm.bits);
    if (alac_) {
      Put(header, Tag("kuki"));
      Put<std::int64_t>(header, 24);
      Append(header, description.initialization_data);
    }
    Put(header, Tag("data"));
    data_size_offset_ = header.size();
    Put<std::int64_t>(header, 0);
    data_payload_ = header.size();
    Put(header, 0u);
    this->output.Write(header);
  }

  bool SupportsTrim() const override { return alac_.has_value(); }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    const auto end = AddOffset(output.Position(), bytes.size());
    if (end - data_payload_ > INT64_MAX) Fail(ContainerErrorCode::LimitExceeded, "CAF data exceeds its 64-bit chunk length");
    unsigned frames;
    if (alac_) {
      if (tail_trim_ || packet.sync_sample == false)
        Fail(WriteError, "CAF ALAC requires independent packets and a continuous coded clock");
      if (packets_.size() == MaxContainerSamples)
        Fail(ContainerErrorCode::LimitExceeded, "CAF packet index exceeds its entry limit");
      frames = AlacPacketSamples(bytes, *alac_, WriteError);
      const auto authored = packet.duration ? ExactTicks(*packet.duration, rate_) : static_cast<std::int64_t>(frames);
      if (authored <= 0 || authored > frames) Fail(WriteError, "CAF ALAC timing exceeds its encoded sample count");
      ValidateAudioPacketTime(packet, frames_, static_cast<unsigned>(authored), rate_);
      packets_.push_back({static_cast<std::uint32_t>(bytes.size()), frames});
      tail_trim_ = frames - authored;
    } else {
      frames = PcmPacketFrames(packet, alignment_, frames_, rate_);
    }
    const auto next = AddTime(frames_, frames);
    output.Write(bytes);
    frames_ = next;
    return {};
  }

  void Finish() override {
    const auto size = static_cast<std::int64_t>(output.Position() - data_payload_);
    if (alac_) FinishAlac();
    Patch(data_size_offset_, size);
  }

private:
  template<class T> void Patch(std::uint64_t offset, T value) {
    Bytes field;
    Put(field, value);
    output.Patch(offset, field);
  }

  void FinishAlac() {
    const auto& track = tracks.front();
    ValidateTrimFields(track);
    const auto declared_end = frames_ - tail_trim_;
    auto priming = static_cast<std::int64_t>(track.priming_samples.value_or(0));
    auto end = declared_end - static_cast<std::int64_t>(track.padding_samples.value_or(0));
    if (!track.edits.empty()) {
      std::vector<MediaPresentationSlice> slices;
      const auto mapped = MapToPresentation(track.edits, {0, rate_}, {declared_end, rate_}, slices);
      if (!mapped.Succeeded()) throw mapped.Error();
      if (slices.empty()) Fail(WriteError, "CAF audio edit does not intersect its packet coverage");
      const auto& slice = slices.front();
      const auto scale = slice.media_start.timescale;
      const auto length = SubTime(ExactTicks(slice.presentation.end, scale), ExactTicks(slice.presentation.start, scale));
      const auto first = CeilSample(slice.media_start, rate_);
      const auto stop = CeilSample({AddTime(slice.media_start.ticks, length), scale}, rate_);
      if ((track.priming_samples && first != priming) || (track.padding_samples && stop != end))
        Fail(WriteError, "CAF audio trim conflicts with the presentation interval");
      priming = first;
      end = stop;
    }
    if (priming < 0 || end > declared_end || end < priming || (!packets_.empty() && end == priming))
      Fail(WriteError, "CAF audio trim exceeds its encoded samples");
    const auto nominal = alac_->frame_length;
    const bool variable = !packets_.empty() && std::ranges::any_of(
        std::ranges::subrange(packets_.begin(), std::prev(packets_.end())), [nominal](auto packet) { return packet.frames != nominal; });
    const auto table_frames = variable ? frames_ : static_cast<std::int64_t>(packets_.size()) * nominal;
    if (priming > INT32_MAX || table_frames - end > INT32_MAX)
      Fail(ContainerErrorCode::Unsupported, "CAF trim exceeds its packet table fields");
    Bytes table;
    Put(table, static_cast<std::int64_t>(packets_.size()));
    Put(table, end - priming);
    Put(table, static_cast<std::int32_t>(priming));
    Put(table, static_cast<std::int32_t>(table_frames - end));
    for (const auto& packet : packets_) {
      PutPacketValue(table, packet.size);
      if (variable) PutPacketValue(table, packet.frames);
    }
    if (variable) Patch(packet_frames_offset_, 0u);
    Bytes header;
    Put(header, Tag("pakt"));
    Put(header, static_cast<std::int64_t>(table.size()));
    output.Write(header);
    output.Write(table);
  }

  struct Packet { std::uint32_t size, frames; };
  std::optional<AlacConfig> alac_;
  std::vector<Packet> packets_;
  std::uint32_t rate_, alignment_;
  std::uint64_t packet_frames_offset_, data_size_offset_, data_payload_;
  std::int64_t frames_ = 0, tail_trim_ = 0;
};

} // namespace

std::unique_ptr<ContainerReader> OpenCafReader(ContainerInput input) {
  if (!input.Random() || !input.Size()) Fail(ContainerErrorCode::NeedRandomAccess, "CAF requires a sized random-access input");
  const auto file_size = *input.Size();
  const auto header = input.ReadBytes(8);
  if (Number<std::uint16_t>(header, 4) != 1 || Number<std::uint16_t>(header, 6))
    Fail(ContainerErrorCode::Unsupported, "Unsupported CAF header version or flags");
  const auto desc = input.ReadBytes(12);
  if (Number<std::uint32_t>(desc) != Tag("desc") || Number<std::int64_t>(desc, 4) != 32)
    Fail(Invalid, "CAF requires an Audio Description chunk first");
  const auto description = ReadDescription(input.ReadBytes(32));
  std::optional<Chunk> data, table, cookie;
  std::size_t chunks = 0;
  while (input.Position() < file_size) {
    if (++chunks > 4096) Fail(ContainerErrorCode::LimitExceeded, "CAF chunk discovery exceeds its budget");
    if (file_size - input.Position() < 12) Fail(Invalid, "Truncated CAF chunk header");
    const auto chunk = input.ReadBytes(12);
    const auto type = Number<std::uint32_t>(chunk);
    const auto size = Number<std::int64_t>(chunk, 4);
    const auto offset = input.Position();
    const bool to_end = size == -1 && type == Tag("data");
    if (!to_end && (size < 0 || static_cast<std::uint64_t>(size) > file_size - offset))
      Fail(Invalid, "CAF chunk exceeds its input");
    const auto length = to_end ? file_size - offset : static_cast<std::uint64_t>(size);
    if (type == Tag("data")) {
      if (data || length < 4) Fail(Invalid, "Invalid CAF Audio Data chunk");
      data = Chunk{offset + 4, length - 4};
    } else if (type == Tag("pakt")) {
      if (table || length < 24) Fail(Invalid, "Invalid CAF Packet Table chunk");
      table = Chunk{offset, length};
    } else if (type == Tag("kuki")) {
      if (cookie) Fail(Invalid, "Duplicate CAF Magic Cookie chunk");
      cookie = Chunk{offset, length};
    } else if (type == Tag("desc")) {
      Fail(Invalid, "Duplicate CAF Audio Description chunk");
    }
    input.Seek(offset + length);
  }
  if (!data) Fail(Invalid, "CAF is missing its Audio Data chunk");
  if (description.format == Tag("alac")) {
    if (!cookie || !table || (cookie->size != 24 && cookie->size != 48))
      Fail(Invalid, "CAF ALAC needs a cookie and packet table");
    if (table->size > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "CAF packet table exceeds its byte budget");
    input.Seek(cookie->offset);
    const auto bytes = input.ReadBytes(static_cast<std::size_t>(cookie->size));
    input.Seek(table->offset);
    const auto packets = input.ReadBytes(static_cast<std::size_t>(table->size));
    return std::make_unique<CafAlacReader>(std::move(input), description, *data, AlacCookie(bytes, description.channels), packets);
  }
  if (description.format != Tag("lpcm") || description.flags & ~3u || description.packet_frames != 1)
    Fail(ContainerErrorCode::Unsupported, "Unsupported CAF audio description");
  const auto codec = SignedPcmCodec(description.bits, !(description.flags & 2), (description.flags & 1) != 0);
  const auto alignment = description.channels * (description.bits / 8);
  if (alignment != description.packet_bytes)
    Fail(ContainerErrorCode::Unsupported, "CAF LPCM requires packed interleaved samples");
  if (data->size % alignment) Fail(Invalid, "CAF LPCM data splits a sample frame");
  if (table) {
    input.Seek(table->offset);
    const PacketTable trim(input.ReadBytes(24));
    if (static_cast<std::uint64_t>(trim.Total()) != data->size / alignment)
      Fail(Invalid, "CAF LPCM sample count differs from the packet table");
    data->offset += static_cast<std::uint64_t>(trim.priming) * alignment;
    data->size = static_cast<std::uint64_t>(trim.valid) * alignment;
  }
  input.Seek(data->offset);
  return std::make_unique<PcmReader>(std::move(input), ContainerFormat::Caf,
      PcmTrack(codec, description.rate, description.channels), data->size, alignment);
}

void ValidateCaf(std::span<const MediaTrack> tracks) {
  const auto& track = tracks.front();
  const auto& description = track.descriptions.front();
  if (tracks.size() != 1 || track.kind != TrackKind::Audio || description.framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "CAF requires one LPCM or ALAC track");
  if (description.codec != CodecId::Alac) {
    if (description.codec == CodecId::PcmU8) Fail(ContainerErrorCode::Unsupported, "CAF integer PCM uses signed samples");
    ValidatePcmTrack(track);
    return;
  }
  const auto alac = ParseAlac(description.initialization_data, WriteError);
  if (description.sample_rate != alac.sample_rate || description.channel_count != alac.channels)
    Fail(WriteError, "CAF ALAC track metadata differs from its cookie");
  if (track.edits.size() > 1 || (!track.edits.empty() && !track.edits.front().media_start))
    Fail(ContainerErrorCode::Unsupported, "CAF ALAC requires a single contiguous presentation interval");
  if (!track.edits.empty()) {
    const auto& edit = track.edits.front();
    if (!edit.media_start->timescale || edit.media_start->ticks < 0 || !edit.duration.timescale || edit.duration.ticks <= 0)
      Fail(WriteError, "Invalid CAF ALAC presentation interval");
  }
  ValidateTrimFields(track);
}

std::unique_ptr<ContainerWriter> OpenCafWriter(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<CafWriter>(std::move(output), tracks);
}

} // namespace huxerui::media::detail
