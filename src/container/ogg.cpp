#include "container_io.h"

#include <array>
#include <deque>
#include <numeric>

namespace huxerui::media::detail {
namespace {

constexpr auto Bad = ContainerErrorCode::InvalidData;
constexpr auto Argument = ContainerErrorCode::InvalidArgument;
constexpr std::uint32_t Clock = 48000;
constexpr std::int64_t Preroll = 3840;
constexpr std::uint8_t Continued = 1, Beginning = 2, Last = 4;
constexpr std::uint32_t Serial = 1;
constexpr std::size_t MaxPages = MaxContainerSamples;

constexpr auto CrcTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    auto value = i << 24;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value << 1) ^ ((value & 0x80000000u) ? 0x04c11db7u : 0);
    table[i] = value;
  }
  return table;
}

std::uint32_t Checksum(ByteView bytes, std::uint32_t crc = 0) {
  constexpr auto table = CrcTable();
  for (auto byte : bytes) crc = (crc << 8) ^ table[(crc >> 24) ^ std::to_integer<unsigned>(byte)];
  return crc;
}

struct Page {
  std::uint64_t offset;
  std::uint8_t flags;
  std::int64_t granule;
  std::uint32_t sequence;
  std::vector<Bytes> packets;
};

struct Cursor {
  std::uint32_t sequence = 0;
  std::optional<std::int64_t> granule;
  Bytes partial;
  bool ended = false;
};

Page ReadPage(ContainerInput& input, Cursor& cursor, std::uint32_t& serial) {
  if (input.Peek(1).empty()) Fail(Bad, "Ogg stream ended before an EOS page");
  const auto offset = input.Position();
  auto header = input.ReadBytes(27);
  const auto flags = Number<std::uint8_t>(header, 5);
  const auto page_serial = Number<std::uint32_t>(header, 14, true);
  const auto sequence = Number<std::uint32_t>(header, 18, true);
  if (Number<std::uint32_t>(header) != Tag("OggS") || Number<std::uint8_t>(header, 4) || (flags & ~7u))
    Fail(Bad, "Invalid Ogg capture pattern, version, or flags");
  if (sequence != cursor.sequence || bool(flags & Continued) != !cursor.partial.empty())
    Fail(Bad, "Discontinuous Ogg page sequence or packet continuation");
  if (flags & Beginning) serial = page_serial;
  if (serial != page_serial) Fail(ContainerErrorCode::Unsupported, "Multiplexed Ogg streams are unsupported");
  ++cursor.sequence;
  auto lacing = input.ReadBytes(Number<std::uint8_t>(header, 26));
  const auto length = std::accumulate(lacing.begin(), lacing.end(), 0u, [](unsigned sum, std::byte value) {
    return sum + std::to_integer<unsigned>(value);
  });
  const auto body = input.ReadBytes(length);
  const auto expected = Number<std::uint32_t>(header, 22, true);
  std::ranges::fill(std::span(header).subspan(22, 4), std::byte{});
  if (Checksum(body, Checksum(lacing, Checksum(header))) != expected) Fail(Bad, "Ogg page checksum differs");

  Page page{offset, flags, Number<std::int64_t>(header, 6, true), sequence, {}};
  std::size_t position = 0;
  for (auto lace : lacing) {
    const auto count = std::to_integer<unsigned>(lace);
    if (cursor.partial.size() > MaxContainerPacket - count)
      Fail(ContainerErrorCode::LimitExceeded, "Ogg packet exceeds the size limit");
    Append(cursor.partial, ByteView(body).subspan(position, count));
    position += count;
    if (count < 255) page.packets.push_back(std::exchange(cursor.partial, {}));
  }
  if (page.granule < -1 || (page.packets.empty() != (page.granule == -1)))
    Fail(Bad, "Ogg granule does not match completed packets");
  if ((flags & Last) && (!cursor.partial.empty() || page.packets.empty()))
    Fail(Bad, "Ogg EOS does not complete its last packet");
  return page;
}

struct AudioPage {
  Page page;
  std::vector<unsigned> lengths;
  std::int64_t first = 0;
  std::int64_t padding = 0;
};

constexpr std::size_t MaxLinks = 4096;

struct Link {
  MediaSampleDescription description;
  std::uint16_t pre_skip = 0;
  std::uint32_t serial = 0;
  std::optional<std::int64_t> start;
  std::optional<std::int64_t> first;

  std::int64_t Preskip() const { return pre_skip; }
  std::int64_t Time(std::int64_t granule) const {
    return AddTime(*start, SubTime(SubTime(granule, *first), Preskip()));
  }
  std::int64_t End(const Cursor& cursor) const {
    const auto time = Time(*cursor.granule);
    if (time < *start) Fail(Bad, "Ogg Opus pre-skip exceeds its audible timeline");
    return time;
  }
};

Link ReadLink(ContainerInput& input, Cursor& cursor, std::optional<std::int64_t> start = {}) {
  cursor = {};
  Link link{.start = start};
  auto head = ReadPage(input, cursor, link.serial);
  if (head.flags != Beginning || head.granule || head.packets.size() != 1 || !cursor.partial.empty())
    Fail(Bad, "Invalid Ogg Opus identification page");
  const auto info = ParseOpus(head.packets.front(), ContainerErrorCode::Unsupported);

  auto tags = ReadPage(input, cursor, link.serial);
  for (std::size_t pages = 0; tags.packets.empty() && !(tags.flags & (Beginning | Last)); ++pages) {
    if (pages == 1024) Fail(ContainerErrorCode::LimitExceeded, "OpusTags exceeds its page limit");
    tags = ReadPage(input, cursor, link.serial);
  }
  if ((tags.flags & (Beginning | Last)) || tags.granule || tags.packets.size() != 1 || !cursor.partial.empty())
    Fail(Bad, "Invalid Ogg Opus comment pages");
  const auto comments = ByteView(tags.packets.front());
  if (comments.size() < 16 || Number<std::uint64_t>(comments) != 0x4f70757354616773ull)
    Fail(Bad, "Missing OpusTags packet");
  auto fields = comments.subspan(8);
  const auto skip_string = [&] {
    const auto length = Number<std::uint32_t>(fields, 0, true);
    fields = fields.subspan(4);
    Slice(fields, 0, length);
    fields = fields.subspan(length);
  };
  skip_string();
  const auto count = Number<std::uint32_t>(fields, 0, true);
  fields = fields.subspan(4);
  if (count > fields.size() / 4) Fail(Bad, "Invalid OpusTags comment count");
  for (std::uint32_t i = 0; i < count; ++i) skip_string();

  link.description = {.codec = CodecId::Opus, .initialization_data = std::move(head.packets.front()),
                      .sample_rate = Clock, .channel_count = info.channels};
  link.pre_skip = info.pre_skip;
  return link;
}

bool HasNextLink(ContainerInput& input) {
  const auto next = input.Peek(6);
  if (next.empty()) return false;
  if (next.size() == 6 && Number<std::uint32_t>(next) == Tag("OggS") &&
      Number<std::uint8_t>(next, 4) == 0 && (Number<std::uint8_t>(next, 5) & Beginning)) return true;
  Fail(Bad, "Trailing data after Ogg EOS");
}

AudioPage ReadAudioPage(ContainerInput& input, Cursor& cursor, Link& link) {
  auto serial = link.serial;
  AudioPage audio{ReadPage(input, cursor, serial), {}};
  if (audio.page.flags & Beginning) Fail(ContainerErrorCode::Unsupported, "An Ogg link began before the current EOS");
  if (audio.page.packets.empty()) return audio;
  std::int64_t samples = 0;
  for (auto& packet : audio.page.packets) {
    const auto length = OpusPacketSamples(packet, Bad);
    audio.lengths.push_back(length);
    samples += length;
  }
  cursor.ended = audio.page.flags & Last;
  audio.first = cursor.granule.value_or(std::max<std::int64_t>(0, audio.page.granule - samples));
  const auto untrimmed = AddTime(audio.first, samples);
  if (cursor.ended) {
    if (audio.page.granule < link.Preskip() || audio.page.granule < audio.first ||
        audio.page.granule > untrimmed || untrimmed - audio.page.granule > audio.lengths.back())
      Fail(Bad, "Ogg Opus EOS has invalid tail trim");
    audio.padding = untrimmed - audio.page.granule;
  } else if (audio.page.granule != untrimmed) {
    Fail(Bad, "Ogg Opus granule does not match the packet durations");
  }
  cursor.granule = audio.page.granule;
  if (!link.first) link.first = audio.first;
  if (!link.start) link.start = audio.first;
  return audio;
}

class OggReader final : public ContainerReader {
public:
  explicit OggReader(ContainerInput input) : ContainerReader(std::move(input), ContainerFormat::Ogg) {
    origin_ = this->input.Position();
    link_ = ReadLink(this->input, cursor_);
    tracks.push_back({.id = 1, .kind = TrackKind::Audio, .timescale = Clock,
        .descriptions = {link_.description}, .priming_samples = link_.pre_skip,
        .seek_preroll = MediaTimestamp{Preroll, Clock}});
  }

  std::optional<ContainerPacket> Read() override {
    while (ready_.empty()) {
      if (cursor_.ended) {
        if (!HasNextLink(input)) return {};
        if (++links_read_ > MaxLinks) Fail(ContainerErrorCode::LimitExceeded, "Ogg chain exceeds its link limit");
        link_ = ReadLink(input, cursor_, link_.End(cursor_));
        description_index_ = RegisterAudioDescription(tracks.front(), link_.description);
        active_descriptions[0] = 0;
      }
      auto audio = ReadAudioPage(input, cursor_, link_);
      if (audio.lengths.empty()) continue;
      if (!start) start = MediaTimestamp{*link_.start, Clock};
      first_audible_pts[0] = MediaTimestamp{*link_.start, Clock};
      auto time = link_.Time(audio.first);
      for (std::size_t i = 0; i < audio.lengths.size(); ++i) {
        auto owner = std::make_shared<const Bytes>(std::move(audio.page.packets[i]));
        ContainerPacket packet{1, BufferReference(*owner, owner), MediaTimestamp{time, Clock},
                               {}, MediaTimestamp{audio.lengths[i], Clock}, true, {}, description_index_};
        if (audio.padding && i + 1 == audio.lengths.size())
          packet.discard_padding = MediaTimestamp{audio.padding, Clock};
        ready_.push_back(std::move(packet));
        time = AddTime(time, audio.lengths[i]);
      }
      if (cursor_.ended) {
        const auto end = link_.End(cursor_);
        if (!duration && input.Peek(1).empty()) duration = MediaTimestamp{SubTime(end, start->ticks), Clock};
      }
    }
    auto packet = std::move(ready_.front());
    ready_.pop_front();
    return packet;
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp target, SeekDirection direction, bool seek) override {
    if (!input.Random()) return ContainerReader::Find(id, target, direction, seek);
    BuildIndex();
    auto link = std::upper_bound(index_.begin(), index_.end(), target,
        [](MediaTimestamp time, const IndexedLink& entry) { return time.Compare({*entry.link.start, Clock}) < 0; });
    if (link != index_.begin()) --link;
    for (; link != index_.end(); ++link) {
      const auto& anchors = link->anchors;
      const auto presented = [&](const Anchor& anchor) {
        return MediaTimestamp{std::max(anchor.time, *link->link.start), Clock};
      };
      auto point = std::lower_bound(anchors.begin(), anchors.end(), target, [&](const Anchor& anchor, MediaTimestamp time) {
        return presented(anchor).Compare(time) < 0;
      });
      if (direction == SeekDirection::AtOrBefore && (point == anchors.end() || presented(*point) != target)) {
        if (point == anchors.begin()) return {};
        --point;
      }
      if (point == anchors.end()) continue;
      const auto preroll = MediaTimestamp{SubTime(point->time, Preroll), Clock};
      auto decode = std::upper_bound(anchors.begin(), point + 1, preroll,
          [](MediaTimestamp time, const Anchor& anchor) { return time.Compare({anchor.time, Clock}) < 0; });
      if (decode != anchors.begin()) --decode;
      if (seek) {
        input.Seek(decode->offset);
        cursor_ = Cursor{.sequence = decode->sequence, .granule = decode->before};
        link_ = link->link;
        description_index_ = RegisterAudioDescription(tracks.front(), link_.description);
        links_read_ = static_cast<std::size_t>(link - index_.begin()) + 1;
        ready_.clear();
      }
      return SeekPoint{id, presented(*point), SeekPoint::Kind::NeedsPreroll, point->offset,
                       MediaTimestamp{decode->time, Clock}};
    }
    return {};
  }

private:
  struct Anchor {
    std::uint64_t offset;
    std::uint32_t sequence;
    std::optional<std::int64_t> before;
    std::int64_t time;
  };

  struct IndexedLink {
    Link link;
    std::vector<Anchor> anchors;
  };

  void BuildIndex() {
    if (!index_.empty()) return;
    const auto previous = input.Position();
    std::vector<IndexedLink> index;
    std::optional<std::int64_t> end;
    std::size_t pages = 0;
    try {
      input.Seek(origin_);
      do {
        if (index.size() == MaxLinks) Fail(ContainerErrorCode::LimitExceeded, "Ogg chain exceeds its link limit");
        Cursor cursor;
        IndexedLink entry{ReadLink(input, cursor, end), {}};
        while (!cursor.ended) {
          if (pages++ == MaxPages) Fail(ContainerErrorCode::LimitExceeded, "Ogg index exceeds its page limit");
          const auto before = cursor.granule;
          const auto audio = ReadAudioPage(input, cursor, entry.link);
          if (!audio.lengths.empty() && !(audio.page.flags & Continued))
            entry.anchors.push_back({audio.page.offset, audio.page.sequence, before, entry.link.Time(audio.first)});
        }
        if (entry.anchors.empty()) Fail(ContainerErrorCode::IndexUnavailable, "Ogg link has no restartable page");
        end = entry.link.End(cursor);
        index.push_back(std::move(entry));
      } while (HasNextLink(input));
    } catch (...) {
      input.Seek(previous);
      throw;
    }
    input.Seek(previous);
    start = MediaTimestamp{*index.front().link.start, Clock};
    duration = MediaTimestamp{SubTime(*end, start->ticks), Clock};
    index_ = std::move(index);
  }

  Cursor cursor_;
  Link link_;
  std::uint32_t description_index_ = 1;
  std::uint64_t origin_ = 0;
  std::size_t links_read_ = 1;
  std::deque<ContainerPacket> ready_;
  std::vector<IndexedLink> index_;
};

void WritePage(ContainerOutput& out, ByteView packet, std::uint8_t flags,
               std::int64_t granule, std::uint32_t sequence) {
  Bytes header;
  Put(header, Tag("OggS"));
  Put<std::uint8_t>(header, 0);
  Put(header, flags);
  Put(header, granule, true);
  Put(header, Serial, true);
  Put(header, sequence, true);
  Put(header, 0u, true);
  Put(header, static_cast<std::uint8_t>(packet.size() / 255 + 1));
  header.insert(header.end(), packet.size() / 255, std::byte{255});
  header.push_back(std::byte(packet.size() % 255));
  const auto crc = Checksum(packet, Checksum(header));
  for (unsigned i = 0; i < 4; ++i) header[22 + i] = std::byte((crc >> (8 * i)) & 255);
  out.Write(header);
  out.Write(packet);
}

class OggWriter final : public ContainerWriter {
public:
  OggWriter(ContainerOutput output, std::span<const MediaTrack> tracks)
      : ContainerWriter(std::move(output), tracks), preskip_(ParseOpus(tracks.front().descriptions.front().initialization_data, Argument).pre_skip) {
    WritePage(this->output, tracks.front().descriptions.front().initialization_data, Beginning, 0, sequence_++);
    Bytes tags;
    Put(tags, Tag("Opus"));
    Put(tags, Tag("Tags"));
    Put(tags, 0u, true);
    Put(tags, 0u, true);
    WritePage(this->output, tags, 0, 0, sequence_++);
  }

  bool SupportsPacketTrim() const override { return true; }

  std::optional<std::uint32_t> Write(std::uint32_t, const ContainerPacket& packet) override {
    const auto bytes = packet.data.AsBytes();
    if (bytes.size() >= 255 * 255) Fail(Argument, "Ogg Opus output requires one packet to fit on a page");
    const auto length = OpusPacketSamples(bytes, Argument);
    if (packet.duration && *packet.duration != MediaTimestamp{length, Clock})
      Fail(Argument, "Ogg Opus duration differs from the packet");
    if (packet.pts) {
      const auto time = packet.pts->Rescale(Clock);
      const auto expected = granule_ - preskip_;
      if (!time || time->ticks < expected - 48 ||
          (expected <= INT64_MAX - 48 && time->ticks > expected + 48))
        Fail(ContainerErrorCode::Unsupported, "Ogg Opus requires a continuous 48 kHz timeline");
    }
    std::int64_t padding = 0;
    if (packet.discard_padding) {
      const auto trim = *packet.discard_padding;
      const auto samples = trim.Rescale(Clock);
      // Matroska expresses this field in nanoseconds, including rounded fractions of a sample.
      if (!samples || (*samples != trim && (trim.timescale != 1000000000 || samples->Rescale(trim.timescale) != trim)))
        Fail(Argument, "Ogg Opus trim must identify a whole number of samples");
      padding = samples->ticks;
    }
    if (padding < 0 || padding > length || padding_)
      Fail(ContainerErrorCode::Unsupported, "Ogg Opus can trim only the final packet");
    const auto next = AddTime(granule_, length);
    Bytes retained(bytes.begin(), bytes.end());
    if (!pending_.empty()) WritePage(output, pending_, 0, granule_, sequence_++);
    pending_ = std::move(retained);
    granule_ = next;
    padding_ = padding;
    return {};
  }

  void Finish() override {
    if (pending_.empty() || granule_ - padding_ < preskip_)
      Fail(Argument, "Ogg Opus output has no playable granule");
    WritePage(output, pending_, Last, granule_ - padding_, sequence_);
  }

private:
  std::int64_t preskip_;
  std::int64_t granule_ = 0;
  std::int64_t padding_ = 0;
  std::uint32_t sequence_ = 0;
  Bytes pending_;
};
} // namespace

std::unique_ptr<ContainerReader> OpenOggReader(ContainerInput input) {
  return std::make_unique<OggReader>(std::move(input));
}
std::unique_ptr<ContainerWriter> OpenOggWriter(ContainerOutput output, std::span<const MediaTrack> tracks) {
  return std::make_unique<OggWriter>(std::move(output), tracks);
}
void ValidateOgg(std::span<const MediaTrack> tracks) {
  if (tracks.size() != 1 || tracks.front().kind != TrackKind::Audio ||
      tracks.front().descriptions.front().codec != CodecId::Opus || tracks.front().descriptions.front().framing != PacketFraming::CodecPacket)
    Fail(ContainerErrorCode::Unsupported, "Ogg output requires one Opus audio track");
  const auto& track = tracks.front();
  const auto& description = track.descriptions.front();
  const auto info = ParseOpus(description.initialization_data, Argument);
  if (description.channel_count != info.channels || description.sample_rate != Clock ||
      (track.priming_samples && track.priming_samples != info.pre_skip) || track.padding_samples.value_or(0) ||
      (track.seek_preroll && *track.seek_preroll != MediaTimestamp{Preroll, Clock}))
    Fail(Argument, "Ogg Opus track metadata differs from its mapping");
}
} // namespace huxerui::media::detail
