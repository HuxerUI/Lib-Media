#include "ebml.h"

#include "codec/common.h"
#include "mp4.h"

#include <cmath>

namespace huxerui::media::detail {
namespace {
using namespace ebml;

constexpr std::size_t ElementBudget = 65536;

struct TrackState {
  std::uint64_t number = 0;
  std::int64_t delay = 0;
  std::optional<std::int64_t> default_duration;
};

struct BlockFields {
  ByteView data;
  std::optional<std::uint64_t> duration;
  std::optional<std::int64_t> discard;
  bool reference = false;
  bool codec_state = false;
};

BlockFields Group(ByteView bytes) {
  BlockFields group;
  while (!bytes.empty()) {
    const auto field = ReadField(bytes);
    switch (field.id) {
      case Block:
        if (field.data.empty() || !group.data.empty()) Fail(BadData, "Missing or duplicate Matroska Block");
        group.data = field.data;
        break;
      case BlockDuration: group.duration = UInt(field.data); break;
      case ReferenceBlock: group.reference = true; break;
      case DiscardPadding: group.discard = SInt(field.data); break;
      case 0xa4: group.codec_state = true; break;
    }
  }
  if (group.data.empty()) Fail(BadData, "Matroska BlockGroup has no Block");
  return group;
}

struct BlockHeader {
  std::uint64_t track;
  std::int16_t relative_time;
  unsigned flags;
};

BlockHeader ReadBlockHeader(ByteView& bytes) {
  const auto track = ReadVint(bytes).value;
  if (!track) Fail(BadData, "Zero Matroska block track number");
  BlockHeader header{track, Number<std::int16_t>(bytes), Number<std::uint8_t>(bytes, 2)};
  bytes = bytes.subspan(3);
  return header;
}

std::vector<std::size_t> LaceSizes(ByteView& payload, unsigned kind) {
  if (!kind) return {payload.size()};
  const auto count = Number<std::uint8_t>(payload) + 1u;
  payload = payload.subspan(1);
  if (count < 2) Fail(BadData, "Matroska lace contains fewer than two frames");
  if (kind == 2) {
    if (payload.size() % count) Fail(BadData, "Fixed Matroska lace has unequal frame sizes");
    return std::vector<std::size_t>(count, payload.size() / count);
  }

  std::vector<std::size_t> sizes;
  sizes.reserve(count);
  std::uint64_t total = 0;
  for (unsigned i = 0; i < count - 1; ++i) {
    std::uint64_t size = 0;
    if (kind == 1) {
      for (;;) {
        const auto octet = Number<std::uint8_t>(payload);
        payload = payload.subspan(1);
        size = AddOffset(size, octet);
        if (octet != 255) break;
      }
    } else {
      const auto field = ReadVint(payload);
      if (!i) {
        size = field.value;
      } else {
        // EBML lace deltas are biased VINTs relative to the preceding frame size.
        const auto bias = (std::uint64_t{1} << (7 * field.width - 1)) - 1;
        const auto delta = SubTime(SignedTime(field.value), SignedTime(bias));
        const auto next = AddTime(static_cast<std::int64_t>(sizes.back()), delta);
        if (next <= 0) Fail(BadData, "Invalid Matroska lace frame size");
        size = static_cast<std::uint64_t>(next);
      }
    }
    total = AddOffset(total, size);
    if (!size || total >= payload.size()) Fail(BadData, "Matroska lace sizes exceed the Block");
    sizes.push_back(static_cast<std::size_t>(size));
  }
  sizes.push_back(payload.size() - static_cast<std::size_t>(total));
  return sizes;
}

std::uint32_t Dimension(ByteView data) {
  const auto value = UInt(data);
  if (value > std::numeric_limits<std::uint32_t>::max()) Fail(BadData, "Matroska dimension exceeds its range");
  return static_cast<std::uint32_t>(value);
}

void ReadVideo(ByteView fields, MediaTrack& track) {
  std::uint32_t top = 0, bottom = 0, left = 0, right = 0;
  while (!fields.empty()) {
    const auto field = ReadField(fields);
    switch (field.id) {
      case 0xb0: track.coded_width = Dimension(field.data); break;
      case 0xba: track.coded_height = Dimension(field.data); break;
      case 0x54aa: bottom = Dimension(field.data); break;
      case 0x54bb: top = Dimension(field.data); break;
      case 0x54cc: left = Dimension(field.data); break;
      case 0x54dd: right = Dimension(field.data); break;
    }
  }
  const auto width = track.coded_width.value_or(0), height = track.coded_height.value_or(0);
  if (!width || !height || left >= width || right >= width - left || top >= height || bottom >= height - top)
    Fail(BadData, "Invalid Matroska video bounds");
  if (left || right || top || bottom) track.visible = VideoRectangle{left, top, width - left - right, height - top - bottom};
}

void ReadAudio(ByteView fields, MediaTrack& track) {
  track.channel_count = 1;
  track.sample_rate = 8000;
  while (!fields.empty()) {
    const auto field = ReadField(fields);
    if (field.id == 0x9f) track.channel_count = Dimension(field.data);
    if (field.id == 0xb5) {
      const auto rate = Float(field.data);
      if (rate < 1 || rate > std::numeric_limits<std::uint32_t>::max() || std::trunc(rate) != rate)
        Fail(Unsupported, "Unsupported Matroska sampling frequency");
      track.sample_rate = static_cast<std::uint32_t>(rate);
    }
  }
}

struct Cue {
  std::int64_t ticks;
  std::uint64_t cluster;
  std::optional<std::uint64_t> block;
};

struct Anchor {
  std::uint64_t cluster;
  std::uint64_t block;
  std::int64_t time;
  bool sync;
};

class MatroskaReader final : public ContainerReader {
public:
  MatroskaReader(ContainerInput source, std::optional<ContainerFormat> hint)
      : ContainerReader(std::move(source), ContainerFormat::Matroska) {
    const auto header = ReadElement(input);
    if (!header || header->id != Header) Fail(BadData, "Missing EBML header");
    const auto bytes = ReadBody(input, *header, 4096);
    ByteView fields(bytes);
    std::string_view type;
    std::uint64_t read_version = 1;
    while (!fields.empty()) {
      const auto field = ReadField(fields);
      if (field.id == 0x4282) type = Text(field.data);
      if (field.id == 0x4285) read_version = UInt(field.data);
      if ((field.id == 0x42f7 && UInt(field.data) > 1) ||
          (field.id == 0x42f2 && UInt(field.data) > 4) ||
          (field.id == 0x42f3 && UInt(field.data) > 8))
        Fail(Unsupported, "Unsupported EBML header version or limits");
    }
    if (type == "webm") format = ContainerFormat::WebM;
    else if (type != "matroska") Fail(ContainerErrorCode::UnrecognizedFormat, "Unrecognized EBML document type");
    if (hint && *hint != format) Fail(ContainerErrorCode::UnrecognizedFormat, "EBML document does not match format hint");
    if (!read_version || read_version > 4) Fail(Unsupported, "Unsupported Matroska document read version");

    std::optional<Element> segment;
    while (auto element = ReadElement(input)) {
      if (element->id == Segment) {
        segment = element;
        break;
      }
      if (element->id != Void || input.Position() > MaxContainerMetadata)
        Fail(BadData, "Missing Matroska Segment");
      SkipElement(input, *element);
    }
    if (!segment) Fail(BadData, "Missing Matroska Segment");
    segment_data_ = segment->data;
    segment_end_ = segment->end ? segment->end : input.Size();
    DiscoverTracks();
  }

  std::optional<DemuxItem> Read() override {
    for (;;) {
      if (lace_cursor_ < lace_.size()) return DemuxItem(std::move(lace_[lace_cursor_++]));
      lace_.clear();
      lace_cursor_ = 0;
      auto element = Next();
      if (!element) return {};
      if (!in_cluster_) {
        if (element->id == Cluster) {
          in_cluster_ = true;
          cluster_end_ = element->end;
          cluster_time_.reset();
        } else if (element->id == Segment || element->id == Header) {
          Fail(Unsupported, "Multiple Matroska Segments are unsupported");
        } else {
          SkipElement(input, *element);
        }
        continue;
      }
      if (element->id == Timestamp) {
        if (cluster_time_) Fail(BadData, "Duplicate Cluster timestamp");
        cluster_time_ = SignedTime(UInt(ReadBody(input, *element, 8)));
        continue;
      }
      if (element->id == SimpleBlock || element->id == BlockGroup) {
        if (!cluster_time_) Fail(BadData, "Matroska Cluster has no timestamp before its Blocks");
        if (seek_offset_ && element->position < *seek_offset_) {
          SkipElement(input, *element);
        } else {
          seek_offset_.reset();
          ReadPackets(*element, *cluster_time_);
        }
      } else {
        SkipElement(input, *element);
      }
    }
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override {
    if (!input.Random()) return ContainerReader::Find(id, time, direction, seek);
    const auto track = PacketTrack(tracks, id);
    if (tracks[track].codec == CodecId::Unknown) Fail(Unsupported, "No packet mapping for the requested Matroska track");

    auto query = [&]() -> std::optional<std::pair<SeekPoint, Anchor>> {
      LoadCues();
      const auto& index = cues_[track];
      if (index.empty()) Fail(ContainerErrorCode::IndexUnavailable, "No Matroska Cues for the requested track");
      const auto after = direction == SeekDirection::AtOrAfter;
      auto at = after
          ? std::lower_bound(index.begin(), index.end(), time, [&](const Cue& cue, MediaTimestamp target) {
              return MediaTimestamp{Time(cue.ticks, track), Nanoseconds}.Compare(target) < 0;
            })
          : std::upper_bound(index.begin(), index.end(), time, [&](MediaTimestamp target, const Cue& cue) {
              return target.Compare({Time(cue.ticks, track), Nanoseconds}) < 0;
            });
      if ((after && at == index.end()) || (!after && at == index.begin())) return {};
      if (!after) --at;
      auto anchor = Locate(at->cluster, track, at->ticks, at->block);
      if (!anchor) Fail(BadData, "Matroska Cue does not point to a matching Block");
      if (tracks[track].kind == TrackKind::Video && !anchor->sync)
        Fail(ContainerErrorCode::IndexUnavailable, "Matroska Cue does not identify a video sync block");
      SeekPoint point{id, {anchor->time, Nanoseconds}, SeekPoint::Kind::SyncMarked, anchor->block, {}};
      auto decode = *anchor;
      if (tracks[track].seek_preroll && tracks[track].seek_preroll->ticks > 0) {
        const auto desired = SubTime(anchor->time, ToNs(*tracks[track].seek_preroll));
        const auto start = std::upper_bound(index.begin(), index.end(), desired, [&](auto value, const Cue& cue) {
          return value < Time(cue.ticks, track);
        });
        // Before the first indexed preroll point, start at the first Block of this track.
        std::optional<Anchor> earlier;
        if (start != index.begin()) {
          const auto& cue = *std::prev(start);
          earlier = Locate(cue.cluster, track, cue.ticks, cue.block);
        } else if (first_cluster_) {
          earlier = Locate(*first_cluster_, track, {});
        }
        if (!earlier || earlier->time > anchor->time)
          Fail(ContainerErrorCode::IndexUnavailable, "Matroska index does not cover the required preroll");
        decode = *earlier;
        point.kind = SeekPoint::Kind::NeedsPreroll;
        point.decode_start = MediaTimestamp{decode.time, Nanoseconds};
      }
      return std::pair{point, decode};
    };

    const auto previous = input.Position();
    std::optional<std::pair<SeekPoint, Anchor>> result;
    try {
      result = query();
    } catch (...) {
      input.Seek(previous);
      throw;
    }
    if (!seek || !result) {
      input.Seek(previous);
    } else {
      input.Seek(result->second.cluster);
      seek_offset_ = result->second.block;
      pending_.reset();
      in_cluster_ = false;
      cluster_end_.reset();
      lace_.clear();
      lace_cursor_ = 0;
    }
    if (!result) return {};
    return result->first;
  }

private:
  void DiscoverTracks() {
    bool info = false;
    std::optional<std::uint64_t> tracks_position, info_position;
    std::size_t boxes = 0;
    while (auto element = ReadElement(input, segment_end_)) {
      if (++boxes > ElementBudget || element->position - segment_data_ > MaxContainerMetadata)
        Fail(ContainerErrorCode::LimitExceeded, "Matroska header discovery exceeds its budget");
      if (element->id == Cluster) {
        first_cluster_ = element->position;
        pending_ = element;
        break;
      }
      if (element->id == Info) {
        if (info) Fail(BadData, "Duplicate Matroska Info");
        ReadInfo(ReadBody(input, *element));
        info = true;
      } else if (element->id == Tracks) {
        if (!tracks.empty()) Fail(BadData, "Duplicate Matroska Tracks");
        ReadTracks(ReadBody(input, *element));
      } else if (element->id == SeekHead) {
        const auto body = ReadBody(input, *element);
        ByteView fields(body);
        while (!fields.empty()) {
          const auto entry = ReadField(fields);
          if (entry.id != 0x4dbb) continue;
          auto data = entry.data;
          std::optional<std::uint64_t> id, offset;
          while (!data.empty()) {
            const auto field = ReadField(data);
            if (field.id == 0x53ab) id = UInt(field.data);
            if (field.id == 0x53ac) offset = UInt(field.data);
          }
          if (!id || !offset) continue;
          if (*id == Cues) cues_position_ = SegmentPosition(*offset);
          if (*id == Tracks) tracks_position = SegmentPosition(*offset);
          if (*id == Info) info_position = SegmentPosition(*offset);
        }
      } else {
        if (element->id == Cues) cues_position_ = element->position;
        SkipElement(input, *element);
      }
    }
    if (!tracks.empty() && info) return;
    if (pending_ && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "Matroska metadata follows its first Cluster");
    const auto position = input.Position();
    if (!info && input.Random() && info_position) {
      ReadInfo(ReadBody(input, IndexedElement(*info_position, Info)));
      info = true;
    }
    if (tracks.empty() && input.Random() && tracks_position)
      ReadTracks(ReadBody(input, IndexedElement(*tracks_position, Tracks)));
    if (!info || tracks.empty()) Fail(BadData, "Missing Matroska Info or Tracks");
    input.Seek(position);
  }

  void ReadInfo(ByteView fields) {
    std::optional<double> length;
    while (!fields.empty()) {
      const auto field = ReadField(fields);
      if (field.id == 0x2ad7b1) scale_ = UInt(field.data);
      if (field.id == 0x4489) length = Float(field.data);
    }
    if (!scale_ || scale_ > std::numeric_limits<std::int64_t>::max())
      Fail(BadData, "Invalid Matroska timestamp scale");
    if (length) {
      const auto nanos = std::round(*length * static_cast<double>(scale_));
      if (nanos < 0 || nanos >= 9223372036854775808.0)
        Fail(BadData, "Invalid Matroska Segment duration");
      duration = MediaTimestamp{static_cast<std::int64_t>(nanos), Nanoseconds};
    }
  }

  void ReadTracks(ByteView fields) {
    while (!fields.empty()) {
      const auto entry = ReadField(fields);
      if (entry.id != 0xae) continue;
      if (tracks.size() == MaxContainerTracks) Fail(ContainerErrorCode::LimitExceeded, "Too many Matroska tracks");
      MediaTrack track;
      TrackState state;
      std::uint64_t type = 0;
      ByteView audio, video;
      std::string_view codec;
      bool encodings = false, custom_clock = false;
      auto data = entry.data;
      while (!data.empty()) {
        const auto field = ReadField(data);
        switch (field.id) {
          case 0xd7: state.number = UInt(field.data); break;
          case 0x83: type = UInt(field.data); break;
          case 0x86: codec = Text(field.data); break;
          case 0x63a2:
            if (field.data.size() > 65536) Fail(ContainerErrorCode::LimitExceeded, "Matroska codec private data exceeds 64 KiB");
            track.initialization_data.assign(field.data.begin(), field.data.end());
            break;
          case 0x23e383: state.default_duration = SignedTime(UInt(field.data)); break;
          case 0x23314f: custom_clock |= Float(field.data) != 1; break;
          case 0x537f: custom_clock |= SInt(field.data) != 0; break;
          case 0x56aa: state.delay = SignedTime(UInt(field.data)); break;
          case 0x56bb: track.seek_preroll = MediaTimestamp{SignedTime(UInt(field.data)), Nanoseconds}; break;
          case 0xe0: video = field.data; break;
          case 0xe1: audio = field.data; break;
          case 0x6d80: encodings = true; break;
        }
      }
      if (!state.number || std::ranges::find(states_, state.number, &TrackState::number) != states_.end() ||
          !type || codec.empty())
        Fail(BadData, "Invalid or duplicate Matroska track");
      if (state.default_duration == 0) Fail(BadData, "Zero Matroska default duration");
      track.id = static_cast<std::uint32_t>(tracks.size() + 1);
      track.kind = type == 1 ? TrackKind::Video : type == 2 ? TrackKind::Audio :
                   type == 17 ? TrackKind::Subtitle : TrackKind::Data;
      if (!encodings && !custom_clock) ReadCodec(track, state, codec, audio, video);
      if (track.codec == CodecId::Unknown) track.framing = PacketFraming::ContainerSample;
      tracks.push_back(std::move(track));
      states_.push_back(state);
    }
  }

  void ReadCodec(MediaTrack& track, const TrackState& state, std::string_view codec, ByteView audio, ByteView video) {
    if (track.kind == TrackKind::Video) {
      if (codec == "V_MPEGH/ISO/HEVC" && format == ContainerFormat::Matroska) {
        HevcNalLength(track.initialization_data, false, BadData);
        track.codec = CodecId::Hevc;
        track.framing = PacketFraming::LengthPrefixedNals;
      } else if (codec == "V_MPEG4/ISO/AVC" && format == ContainerFormat::Matroska) {
        const auto config = ParseAvc(track.initialization_data);
        if (!config.Succeeded()) {
          if (config.Error().code == CodecErrorCode::Unsupported) return;
          Fail(BadData, config.Error().message);
        }
        track.codec = CodecId::H264;
        track.framing = PacketFraming::LengthPrefixedNals;
      } else if (codec == "V_VP8" && format == ContainerFormat::WebM && track.initialization_data.empty()) {
        track.codec = CodecId::Vp8;
      } else if (codec == "V_VP9" && format == ContainerFormat::WebM) {
        track.codec = CodecId::Vp9;
      }
      if (track.codec != CodecId::Unknown) ReadVideo(video, track);
    } else if (track.kind == TrackKind::Audio && codec == "A_OPUS") {
      if (track.initialization_data.size() >= 19 && Number<std::uint8_t>(track.initialization_data, 18) != 0) return;
      const auto opus = ParseOpus(track.initialization_data, BadData);
      ReadAudio(audio, track);
      if (track.channel_count != opus.channels || (opus.input_rate && track.sample_rate != opus.input_rate) ||
          state.delay < opus.Delay() || state.delay - opus.Delay() > 1)
        Fail(BadData, "OpusHead does not match Matroska audio channels, frequency, or CodecDelay");
      track.codec = CodecId::Opus;
      track.sample_rate = 48000;
      track.priming_samples = opus.pre_skip;
    } else if (track.kind == TrackKind::Audio && codec == "A_AAC" && format == ContainerFormat::Matroska) {
      const auto config = AacLcInitialization(track.initialization_data);
      const auto aac = ParseAac(config);
      if (!aac.Succeeded()) return;
      track.initialization_data.resize(config.size());
      ReadAudio(audio, track);
      if (track.sample_rate != aac.Value().sample_rate || track.channel_count != aac.Value().channels)
        Fail(BadData, "Matroska AAC metadata does not match AudioSpecificConfig");
      track.codec = CodecId::Aac;
      if (state.delay) {
        const auto trim = MediaTimestamp{state.delay, Nanoseconds}.Rescale(*track.sample_rate);
        if (!trim || std::abs(SubTime(ToNs(*trim), state.delay)) > 1)
          Fail(Unsupported, "Matroska AAC delay does not align to decoded sample frames");
        track.priming_samples = static_cast<std::uint64_t>(trim->ticks);
      }
    }
  }

  std::optional<Element> Next() {
    auto element = std::exchange(pending_, {});
    if (!element) element = ReadElement(input, cluster_end_ ? cluster_end_ : segment_end_);
    if (!in_cluster_) return element;
    if (!element || (!cluster_end_ && SegmentChild(element->id))) {
      // A level-one sibling closes an unknown-sized Cluster. Its header has already been read.
      in_cluster_ = false;
      cluster_end_.reset();
      if (!element) return ReadElement(input, segment_end_);
    }
    return element;
  }

  void ReadPackets(const Element& element, std::int64_t cluster_ticks) {
    auto owner = Own(ReadBody(input, element, MaxContainerPacket + 4096));
    const auto bytes = owner.AsBytes();
    const bool simple = element.id == SimpleBlock;
    const auto block = simple ? BlockFields{bytes, {}, {}, false, false} : Group(bytes);
    auto payload = block.data;
    const auto header = ReadBlockHeader(payload);
    const auto index = TrackIndex(header.track);
    const auto& track = tracks[index];
    if (!selected.empty() && std::ranges::find(selected, track.id) == selected.end()) return;
    if (track.codec == CodecId::Unknown || block.codec_state)
      Fail(Unsupported, "Unsupported Matroska track or changing codec state");
    const auto sizes = LaceSizes(payload, (header.flags >> 1) & 3);
    const auto ticks = AddTime(cluster_ticks, header.relative_time);
    const auto pts = Time(ticks, index);
    const auto total = block.duration ? std::optional{ScaleTime(SignedTime(*block.duration), scale_)} : std::nullopt;
    const bool sync = simple ? (header.flags & 128) != 0 : !block.reference;
    std::optional<std::int64_t> elapsed{0};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      const auto frame = Slice(payload, 0, sizes[i]);
      if (frame.empty() || frame.size() > MaxContainerPacket) Fail(BadData, "Invalid Matroska packet size");
      auto length = CodecDuration(track, frame, BadData);
      if (!length && states_[index].default_duration) length = MediaTimestamp{*states_[index].default_duration, Nanoseconds};
      if (total && elapsed && i + 1 == sizes.size()) {
        if (*elapsed > *total) Fail(BadData, "Matroska BlockDuration is shorter than its lace");
        length = MediaTimestamp{*total - *elapsed, Nanoseconds};
      }
      std::optional<MediaTimestamp> discard;
      if (block.discard && ((*block.discard < 0 && i == 0) || (*block.discard >= 0 && i + 1 == sizes.size()))) {
        const auto padding = *block.discard;
        if (track.kind != TrackKind::Audio ||
            (length && (padding > ToNs(*length) || padding < -ToNs(*length))))
          Fail(Unsupported, "Matroska discard padding exceeds its audio packet");
        discard = MediaTimestamp{padding, Nanoseconds};
      }
      std::optional<MediaTimestamp> presentation;
      if (elapsed) presentation = MediaTimestamp{AddTime(pts, *elapsed), Nanoseconds};
      const auto offset = static_cast<std::size_t>(frame.data() - bytes.data());
      lace_.push_back({track.id, owner.Slice(offset, frame.size()), presentation, {}, length, sync, discard});
      if (elapsed && length) {
        // AAC has a rational sample clock. Round the accumulated frame time once for each lace position.
        if (track.codec == CodecId::Aac && i + 1 < sizes.size())
          elapsed = ToNs({static_cast<std::int64_t>(1024 * (i + 1)), *track.sample_rate});
        else
          elapsed = AddTime(*elapsed, ToNs(*length));
      } else {
        elapsed.reset();
      }
      payload = payload.subspan(frame.size());
    }
  }

  std::size_t TrackIndex(std::uint64_t number) const {
    const auto found = std::ranges::find(states_, number, &TrackState::number);
    if (found == states_.end()) Fail(BadData, "Matroska Block references an unknown track");
    return static_cast<std::size_t>(found - states_.begin());
  }

  std::int64_t Time(std::int64_t ticks, std::size_t track) const {
    // CodecDelay moves the stored clock to the presentation clock. Packet padding is reported separately.
    return SubTime(ScaleTime(ticks, scale_), states_[track].delay);
  }

  std::uint64_t SegmentPosition(std::uint64_t offset) const {
    const auto position = AddOffset(segment_data_, offset);
    if (segment_end_ && position >= *segment_end_) Fail(BadData, "Matroska index offset exceeds Segment");
    return position;
  }

  Element IndexedElement(std::uint64_t position, std::uint32_t type) {
    input.Seek(position);
    const auto element = ReadElement(input, segment_end_);
    if (!element || element->id != type) Fail(BadData, "Matroska index points to the wrong EBML element");
    return *element;
  }

  std::optional<std::uint64_t> FindCues() {
    input.Seek(segment_data_);
    bool unknown_cluster = false;
    for (std::size_t i = 0; i < ElementBudget; ++i) {
      const auto element = ReadElement(input, segment_end_);
      if (!element || element->id == Segment || element->id == Header) return {};
      if (unknown_cluster && !SegmentChild(element->id)) {
        SkipElement(input, *element);
        continue;
      }
      unknown_cluster = element->id == Cluster && !element->end;
      if (element->id == Cues) return element->position;
      if (!unknown_cluster) SkipElement(input, *element);
    }
    Fail(ContainerErrorCode::IndexUnavailable, "Matroska Cues discovery exceeded its element budget");
  }

  void LoadCues() {
    if (!cues_.empty()) return;
    const auto position = cues_position_ ? cues_position_ : FindCues();
    if (!position) Fail(ContainerErrorCode::IndexUnavailable, "Matroska Segment has no Cues");
    const auto body = ReadBody(input, IndexedElement(*position, Cues));
    ByteView fields(body);
    std::vector<std::vector<Cue>> parsed(tracks.size());
    std::size_t count = 0;
    while (!fields.empty()) {
      const auto entry = ReadField(fields);
      if (entry.id != 0xbb) continue;
      auto point = entry.data;
      std::optional<std::int64_t> ticks;
      while (!point.empty()) {
        const auto field = ReadField(point);
        if (field.id == 0xb3) ticks = SignedTime(UInt(field.data));
      }
      if (!ticks) Fail(BadData, "Matroska Cue has no timestamp");
      point = entry.data;
      while (!point.empty()) {
        auto positions = ReadField(point);
        if (positions.id != 0xb7) continue;
        std::optional<std::uint64_t> number, cluster, block;
        while (!positions.data.empty()) {
          const auto field = ReadField(positions.data);
          if (field.id == 0xf7) number = UInt(field.data);
          if (field.id == 0xf1) cluster = UInt(field.data);
          if (field.id == 0xf0) block = UInt(field.data);
        }
        if (!number || !cluster) Fail(BadData, "Incomplete Matroska Cue track position");
        if (++count > MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "Too many Matroska Cue points");
        parsed[TrackIndex(*number)].push_back({*ticks, SegmentPosition(*cluster), block});
      }
    }
    for (auto& track : parsed) std::stable_sort(track.begin(), track.end(), [](const Cue& a, const Cue& b) {
      return a.ticks < b.ticks;
    });
    cues_ = std::move(parsed);
  }

  std::optional<Anchor> Locate(std::uint64_t cluster, std::size_t track, std::optional<std::int64_t> time,
                               std::optional<std::uint64_t> block = {}) {
    const auto parent = IndexedElement(cluster, Cluster);
    const auto end = parent.end ? parent.end : segment_end_;
    const auto expected = block ? std::optional{AddOffset(parent.data, *block)} : std::nullopt;
    if (expected && end && *expected >= *end) Fail(BadData, "Matroska Cue block offset exceeds its Cluster");
    std::optional<std::int64_t> timestamp;
    for (std::size_t i = 0; i < ElementBudget; ++i) {
      const auto element = ReadElement(input, end);
      if (!element || (!parent.end && SegmentChild(element->id))) return {};
      if (element->position - parent.data > MaxContainerMetadata)
        Fail(ContainerErrorCode::IndexUnavailable, "Matroska Cluster search exceeded its budget");
      if (expected && element->position > *expected) return {};
      if (element->id == Timestamp) {
        timestamp = SignedTime(UInt(ReadBody(input, *element, 8)));
      } else if (element->id == SimpleBlock || element->id == BlockGroup) {
        if (!timestamp) Fail(BadData, "Matroska Cluster has no timestamp before its Blocks");
        if (expected && element->position != *expected) {
          SkipElement(input, *element);
          continue;
        }
        const auto bytes = ReadBody(input, *element, MaxContainerPacket + 4096);
        const bool simple = element->id == SimpleBlock;
        const auto block = simple ? BlockFields{bytes, {}, {}, false, false} : Group(bytes);
        auto data = block.data;
        const auto header = ReadBlockHeader(data);
        const auto ticks = AddTime(*timestamp, header.relative_time);
        if (header.track == states_[track].number && (!time || ticks == *time))
          return Anchor{cluster, element->position, Time(ticks, track), simple ? (header.flags & 128) != 0 : !block.reference};
      } else {
        SkipElement(input, *element);
      }
    }
    Fail(ContainerErrorCode::IndexUnavailable, "Matroska Block search exceeded its element budget");
  }

  std::uint64_t segment_data_ = 0;
  std::optional<std::uint64_t> segment_end_, first_cluster_, cues_position_, cluster_end_, seek_offset_;
  std::uint64_t scale_ = DefaultTimestampScale;
  std::optional<Element> pending_;
  bool in_cluster_ = false;
  std::optional<std::int64_t> cluster_time_;
  std::vector<TrackState> states_;
  std::vector<ContainerPacket> lace_;
  std::size_t lace_cursor_ = 0;
  std::vector<std::vector<Cue>> cues_;
};
} // namespace

std::unique_ptr<ContainerReader> OpenMatroskaReader(ContainerInput input, std::optional<ContainerFormat> hint) {
  return std::make_unique<MatroskaReader>(std::move(input), hint);
}
} // namespace huxerui::media::detail
