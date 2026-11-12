#include "annex_b.h"
#include "codec/common.h"

#include <cstdlib>
#include <map>

namespace huxerui::media::detail {
namespace {

constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto Unsupported = ContainerErrorCode::Unsupported;
constexpr unsigned PacketBytes = 188;
constexpr std::uint32_t Clock = 90000;
constexpr std::int64_t ClockWrap = std::int64_t{1} << 33;

unsigned Octet(ByteView bytes, std::size_t offset) { return Number<std::uint8_t>(bytes, offset); }

std::uint32_t Crc(ByteView bytes) {
  auto value = UINT32_MAX;
  for (auto byte : bytes) {
    value ^= std::uint32_t(std::to_integer<unsigned>(byte)) << 24;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value << 1) ^ ((value & 0x80000000u) ? 0x04c11db7u : 0);
  }
  return value;
}

class Section {
public:
  template<class Visit> void Push(ByteView bytes, bool start, Visit visit) {
    if (start) {
      const auto pointer = Octet(bytes, 0);
      const auto prefix = Slice(bytes, 1, pointer);
      if (!buffer_.empty()) {
        AppendPayload(prefix, visit);
        if (!buffer_.empty()) Fail(Invalid, "Incomplete MPEG-TS table section");
      }
      bytes = bytes.subspan(1 + pointer);
    } else if (buffer_.empty()) return;
    AppendPayload(bytes, visit);
  }

private:
  template<class Visit> void AppendPayload(ByteView bytes, Visit visit) {
    while (!bytes.empty()) {
      if (buffer_.empty() && bytes.front() == std::byte{255}) return;
      std::size_t target = 3;
      if (buffer_.size() >= target) {
        target += Number<std::uint16_t>(buffer_, 1) & 0xfff;
        if (target < 8 || target > 1024) Fail(Invalid, "Invalid MPEG-TS table size");
      }
      const auto count = std::min(target - buffer_.size(), bytes.size());
      Append(buffer_, bytes.first(count));
      bytes = bytes.subspan(count);
      if (target == 3 || buffer_.size() != target) continue;
      if (Crc(buffer_)) Fail(Invalid, "MPEG-TS table CRC differs");
      visit(ByteView(buffer_).first(buffer_.size() - 4));
      buffer_.clear();
    }
  }

  Bytes buffer_;
};

std::int64_t ReadTime(ByteView bytes, unsigned prefix) {
  const auto a = Octet(bytes, 0);
  const auto b = Number<std::uint16_t>(bytes, 1);
  const auto c = Number<std::uint16_t>(bytes, 3);
  if (a >> 4 != prefix || !(a & b & c & 1)) Fail(Invalid, "Invalid MPEG-TS PES timestamp markers");
  return (std::int64_t((a >> 1) & 7) << 30) | (std::int64_t(b >> 1) << 15) | (c >> 1);
}

std::int64_t Unwrap(std::int64_t value, std::optional<std::int64_t> previous) {
  if (!previous) return value;
  value += (*previous / ClockWrap) * ClockWrap;
  if (value - *previous > ClockWrap / 2) value -= ClockWrap;
  if (*previous - value > ClockWrap / 2) value += ClockWrap;
  return value;
}

struct Pes {
  ByteView data;
  std::int64_t pts, dts;
};

Pes ParsePes(ByteView bytes, std::optional<std::int64_t> previous) {
  if (bytes.size() < 14 || (Number<std::uint32_t>(bytes) >> 8) != 1 || (Octet(bytes, 6) & 0xc0) != 0x80)
    Fail(Invalid, "Invalid MPEG-TS PES header");
  const auto length = Number<std::uint16_t>(bytes, 4);
  if (length) bytes = Slice(bytes, 0, std::size_t(length) + 6);
  const auto header = Slice(bytes, 9, Octet(bytes, 8));
  const auto mode = Octet(bytes, 7) >> 6;
  if (mode != 2 && mode != 3) Fail(Unsupported, "MPEG-TS PES requires presentation timestamps");
  const auto dts = Unwrap(mode == 3 ? ReadTime(Slice(header, 5, 5), 1) : ReadTime(header, 2), previous);
  const auto pts = mode == 3 ? Unwrap(ReadTime(header, 3), dts) : dts;
  return {bytes.subspan(9 + header.size()), pts, dts};
}

class TransportReader final : public IndexedImport {
public:
  explicit TransportReader(ContainerInput source) : IndexedImport(std::move(source), ContainerFormat::MpegTs) {
    const auto bytes = ReadImportBytes(input);
    if (bytes.size() % PacketBytes) Fail(Invalid, "MPEG-TS input ends inside a packet");
    for (std::size_t offset = 0; offset < bytes.size(); offset += PacketBytes)
      ParsePacket(ByteView(bytes).subspan(offset, PacketBytes), offset);
    for (auto& [pid, stream] : streams_) {
      Flush(stream);
      if (stream.pending_video) {
        stream.pending_video->packet.duration = stream.last_delta;
        Queue(std::move(*stream.pending_video));
      }
      if (stream.video && stream.video->Description().codec != CodecId::Unknown)
        tracks.push_back({.id = stream.id, .kind = TrackKind::Video, .timescale = Clock,
            .descriptions = {stream.video->Description()}});
      if (stream.audio)
        tracks.push_back({.id = stream.id, .kind = TrackKind::Audio, .timescale = stream.next_audio->timescale,
            .descriptions = {std::move(*stream.audio)}});
    }
    if (tracks.empty()) Fail(Unsupported, "MPEG-TS program has no supported AVC, HEVC, or AAC streams");
    if (packets_.empty()) Fail(Invalid, "MPEG-TS program has no supported packets");
    const auto earliest = std::ranges::min_element(packets_, [](const auto& a, const auto& b) {
      return a.packet.pts->Compare(*b.packet.pts) < 0;
    });
    const MediaTimestamp origin{ExactTicks(*earliest->packet.pts, Clock), Clock};
    std::ranges::stable_sort(packets_, [](const auto& a, const auto& b) { return a.packet.dts->Compare(*b.packet.dts) < 0; });
    for (auto& item : std::exchange(packets_, {})) {
      for (auto* time : {&*item.packet.pts, &*item.packet.dts})
        time->ticks = SubTime(time->ticks, ExactTicks(origin, time->timescale));
      Add(std::move(item.packet), item.offset);
    }
    Complete();
  }

private:
  struct IndexedPacket {
    ContainerPacket packet;
    std::uint64_t offset;
  };
  struct Stream {
    Stream(std::uint32_t id, CodecId codec) : id(id), codec(codec) {
      if (codec != CodecId::Aac) video.emplace(codec);
    }
    std::uint32_t id;
    CodecId codec;
    std::optional<AnnexBTrack> video;
    std::optional<MediaSampleDescription> audio;
    std::optional<MediaTimestamp> next_audio;
    std::optional<std::int64_t> last_dts;
    std::optional<IndexedPacket> pending_video;
    std::optional<MediaTimestamp> last_delta;
    Bytes bytes;
    std::uint64_t offset = 0;
  };

  void Queue(IndexedPacket item) {
    if (packets_.size() == MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "MPEG-TS packet index exceeds its limit");
    packets_.push_back(std::move(item));
  }

  void ReadPat(ByteView table) {
    if (Octet(table, 0) != 0 || table.size() < 8 || (table.size() - 8) % 4)
      Fail(Invalid, "Invalid MPEG-TS program association table");
    if (!(Octet(table, 5) & 1)) return;
    if (Octet(table, 6) || Octet(table, 7)) Fail(Unsupported, "Multi-section MPEG-TS program tables are unsupported");
    std::optional<unsigned> pid;
    unsigned program = 0;
    for (std::size_t i = 8; i < table.size(); i += 4) {
      const auto number = Number<std::uint16_t>(table, i);
      if (!number) continue;
      if (pid) Fail(Unsupported, "MPEG-TS import requires a single program");
      pid = Number<std::uint16_t>(table, i + 2) & 0x1fff;
      program = number;
    }
    if (!pid || (pmt_pid_ && (*pmt_pid_ != *pid || program_ != program)))
      Fail(Unsupported, "MPEG-TS program changed");
    pmt_pid_ = pid;
    program_ = program;
  }

  void ReadPmt(ByteView table) {
    if (Octet(table, 0) != 2 || table.size() < 12 || Number<std::uint16_t>(table, 3) != program_)
      Fail(Invalid, "Invalid MPEG-TS program map");
    if (!(Octet(table, 5) & 1)) return;
    if (Octet(table, 6) || Octet(table, 7)) Fail(Unsupported, "Multi-section MPEG-TS program maps are unsupported");
    if (!map_.empty()) {
      if (!std::ranges::equal(map_, table)) Fail(Unsupported, "MPEG-TS program map changed");
      return;
    }
    map_.assign(table.begin(), table.end());
    const auto descriptors = Number<std::uint16_t>(table, 10) & 0xfff;
    auto entries = Slice(table, 12, table.size() - 12);
    entries = entries.subspan(Slice(entries, 0, descriptors).size());
    while (!entries.empty()) {
      const auto type = Octet(entries, 0);
      const auto pid = Number<std::uint16_t>(entries, 1) & 0x1fff;
      const auto size = Number<std::uint16_t>(entries, 3) & 0xfff;
      entries = entries.subspan(Slice(entries, 0, 5 + size).size());
      const auto codec = type == 0x1b ? CodecId::H264 : type == 0x24 ? CodecId::Hevc :
          type == 0x0f ? CodecId::Aac : CodecId::Unknown;
      if (codec == CodecId::Unknown) continue;
      if (streams_.size() == MaxContainerTracks)
        Fail(ContainerErrorCode::LimitExceeded, "MPEG-TS program has too many supported streams");
      if (!streams_.try_emplace(pid, static_cast<std::uint32_t>(streams_.size() + 1), codec).second)
        Fail(Invalid, "Duplicate MPEG-TS stream PID");
    }
  }

  void ParsePacket(ByteView packet, std::uint64_t offset) {
    if (Octet(packet, 0) != 0x47 || (Octet(packet, 1) & 128))
      Fail(Invalid, "Invalid MPEG-TS packet sync or transport error");
    const bool start = Octet(packet, 1) & 64;
    const auto pid = Number<std::uint16_t>(packet, 1) & 0x1fff;
    const auto control = Octet(packet, 3);
    const auto stream = streams_.find(pid);
    if (pid && pid != pmt_pid_ && stream == streams_.end()) return;
    if ((control & 0xc0) || !(control & 0x30)) Fail(Unsupported, "MPEG-TS packet is scrambled or has invalid adaptation");
    std::size_t data = 4;
    if (control & 0x20) {
      const auto length = Octet(packet, 4);
      Slice(packet, 5, length);
      if (length && (Octet(packet, 5) & 128))
        Fail(Unsupported, "MPEG-TS import requires a continuous program");
      data = 5 + length;
    }
    if (!(control & 0x10) || data >= PacketBytes) return;
    const auto counter = control & 15;
    const auto [entry, fresh] = continuity_.try_emplace(pid, counter);
    if (!fresh && counter != ((entry->second + 1) & 15)) Fail(Invalid, "MPEG-TS continuity counter differs");
    entry->second = counter;
    const auto payload = packet.subspan(data);
    if (!pid) pat_.Push(payload, start, [&](ByteView section) { ReadPat(section); });
    else if (pid == pmt_pid_) pmt_.Push(payload, start, [&](ByteView section) { ReadPmt(section); });
    else {
      auto& state = stream->second;
      if (start) {
        Flush(state);
        state.offset = offset;
      } else if (state.bytes.empty()) return;
      if (payload.size() > MaxContainerPacket - state.bytes.size())
        Fail(ContainerErrorCode::LimitExceeded, "MPEG-TS PES exceeds its byte limit");
      Append(state.bytes, payload);
    }
  }

  void Flush(Stream& stream) {
    if (stream.bytes.empty()) return;
    const auto pes = ParsePes(stream.bytes, stream.last_dts ? stream.last_dts : clock_);
    if (stream.last_dts && pes.dts <= *stream.last_dts)
      Fail(Unsupported, "MPEG-TS decode timestamps must increase");
    stream.last_dts = clock_ = pes.dts;
    if (stream.video) {
      const auto nals = SplitAnnexB(pes.data);
      if (auto packet = stream.video->Convert(nals, stream.id)) {
        packet->pts = MediaTimestamp{pes.pts, Clock};
        packet->dts = MediaTimestamp{pes.dts, Clock};
        if (stream.pending_video) {
          stream.last_delta = MediaTimestamp{SubTime(pes.dts, stream.pending_video->packet.dts->ticks), Clock};
          stream.pending_video->packet.duration = stream.last_delta;
          Queue(std::move(*stream.pending_video));
        }
        stream.pending_video = IndexedPacket{std::move(*packet), stream.offset};
      }
    } else ReadAudio(stream, pes);
    stream.bytes.clear();
  }

  void ReadAudio(Stream& stream, const Pes& pes) {
    auto data = pes.data;
    bool first = true;
    while (!data.empty()) {
      const auto description = DescribeAdts(Slice(data, 0, 7));
      const auto size = (Number<std::uint32_t>(data, 2) >> 5) & 0x1fff;
      const auto header = Octet(data, 1) & 1 ? 7u : 9u;
      if (size <= header) Fail(Invalid, "MPEG-TS ADTS frame has no payload");
      if (stream.audio && stream.audio->initialization_data != description.initialization_data) Fail(Unsupported, "MPEG-TS AAC configuration changed");
      stream.audio = description;
      const auto rate = *description.sample_rate;
      const auto scale = CommonScale(rate, Clock);
      const MediaTimestamp timestamp{ExactTicks({pes.pts, Clock}, scale), scale};
      if (!stream.next_audio) stream.next_audio = timestamp;
      else if (first && std::abs(SubTime(timestamp.ticks, stream.next_audio->ticks)) > scale / Clock)
        Fail(Unsupported, "MPEG-TS AAC PES timestamps are discontinuous");
      first = false;
      const auto payload = Slice(data, header, size - header);
      const MediaTimestamp length{1024, rate};
      Queue({{stream.id, Own(Bytes(payload.begin(), payload.end())), *stream.next_audio, *stream.next_audio, length, true},
          stream.offset});
      stream.next_audio->ticks = AddTime(stream.next_audio->ticks, ExactTicks(length, scale));
      data = data.subspan(size);
    }
  }

  Section pat_, pmt_;
  std::optional<int> pmt_pid_;
  unsigned program_ = 0;
  std::map<int, int> continuity_;
  std::map<int, Stream> streams_;
  std::vector<IndexedPacket> packets_;
  std::optional<std::int64_t> clock_;
  Bytes map_;
};

} // namespace

bool LooksLikeMpegTs(ByteView prefix) {
  return prefix.size() > 2 * PacketBytes && prefix[0] == std::byte{0x47} &&
      prefix[PacketBytes] == std::byte{0x47} && prefix[2 * PacketBytes] == std::byte{0x47};
}

std::unique_ptr<ContainerReader> OpenMpegTsReader(ContainerInput input) {
  return std::make_unique<TransportReader>(std::move(input));
}

} // namespace huxerui::media::detail
