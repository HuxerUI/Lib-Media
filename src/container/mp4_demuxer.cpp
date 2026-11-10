#include "mp4.h"

#include "codec/common.h"

#include <array>
#include <cmath>

namespace huxerui::media::detail {
namespace {

constexpr auto Invalid = ContainerErrorCode::InvalidData;
constexpr auto Unsupported = ContainerErrorCode::Unsupported;

struct BoxView {
  std::uint32_t tag = 0;
  ByteView bytes;
};

BoxView NextBox(ByteView& input) {
  const auto type = Number<std::uint32_t>(input, 4);
  std::uint64_t size = Number<std::uint32_t>(input);
  std::size_t header = 8;
  if (size == 1) {
    size = Number<std::uint64_t>(input, 8);
    header = 16;
  } else if (!size) {
    size = input.size();
  }
  if (size < header || size > input.size()) Fail(Invalid, "Invalid nested MP4 box length");
  auto data = input.subspan(header, static_cast<std::size_t>(size) - header);
  input = input.subspan(static_cast<std::size_t>(size));
  return {type, data};
}

ByteView Child(ByteView input, std::string_view tag, bool required = true) {
  std::optional<ByteView> found;
  while (!input.empty()) {
    const auto child = NextBox(input);
    if (child.tag != Tag(tag)) continue;
    if (found || child.bytes.empty()) Fail(Invalid, "Duplicate or empty MP4 box: " + std::string(tag));
    found = child.bytes;
  }
  if (!found && required) Fail(Invalid, "Missing MP4 box: " + std::string(tag));
  return found.value_or(ByteView{});
}

struct TopBox {
  std::uint32_t tag;
  std::uint64_t offset;
  std::uint64_t size;
  bool to_eof;
};

std::optional<TopBox> ReadBox(ContainerInput& input) {
  const auto offset = input.Position();
  const auto file_size = input.Size();
  if (file_size && offset > *file_size) Fail(Invalid, "MP4 box offset exceeds input size");
  std::array<std::byte, 8> header;
  if (!input.ReadExact(header)) return {};
  const auto tag = Number<std::uint32_t>(header, 4);
  std::uint64_t size = Number<std::uint32_t>(header);
  const bool to_eof = !size && !file_size;
  if (size == 1) {
    size = Number<std::uint64_t>(input.ReadBytes(8));
  } else if (!size) {
    if (file_size) {
      size = *file_size - offset;
    } else if (tag == Tag("mdat")) {
      size = std::numeric_limits<std::uint64_t>::max() - offset;
    } else {
      Fail(Unsupported, "Unbounded MP4 box requires a known input size");
    }
  }
  const auto header_size = input.Position() - offset;
  if (size < header_size || (file_size && size > *file_size - offset))
    Fail(Invalid, "MP4 box exceeds input size");
  return TopBox{tag, offset, size - header_size, to_eof};
}

Bytes ReadMetadata(ContainerInput& input, std::uint64_t size) {
  if (size > MaxContainerMetadata) Fail(ContainerErrorCode::LimitExceeded, "MP4 metadata exceeds 64 MiB");
  return input.ReadBytes(static_cast<std::size_t>(size));
}

unsigned Version(ByteView bytes) { return Number<std::uint32_t>(bytes) >> 24; }

void CheckVersion(ByteView bytes, unsigned maximum) {
  if (Version(bytes) > maximum) Fail(Unsupported, "Unsupported MP4 full box version");
}

std::size_t Entries(ByteView bytes, std::size_t entry_size) {
  auto count = Number<std::uint32_t>(bytes, 4);
  if (count > MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "MP4 table exceeds its entry limit");
  if (Slice(bytes, 8, count * entry_size).size() != bytes.size() - 8)
    Fail(Invalid, "Inconsistent MP4 table length");
  return count;
}

ByteView FindDescriptor(ByteView bytes, unsigned tag) {
  while (!bytes.empty()) {
    const auto type = Number<std::uint8_t>(bytes);
    std::size_t size = 0, position = 1;
    for (;;) {
      const auto octet = Number<std::uint8_t>(bytes, position++);
      size = (size << 7) | (octet & 127);
      if (!(octet & 128)) break;
      if (position == 5) Fail(Invalid, "Invalid MP4 descriptor length");
    }
    const auto data = Slice(bytes, position, size);
    if (type == tag) return data;
    bytes = bytes.subspan(position + size);
  }
  Fail(Invalid, "Missing MP4 decoder descriptor");
}

Bytes AacInitialization(ByteView esds) {
  CheckVersion(esds, 0);
  const auto es = FindDescriptor(esds.subspan(4), 3);
  const auto flags = Number<std::uint8_t>(es, 2);
  std::size_t offset = 3 + ((flags & 128) ? 2 : 0);
  if (flags & 64) offset += Number<std::uint8_t>(es, offset) + 1;
  if (flags & 32) offset += 2;
  Slice(es, 0, offset);
  const auto config = FindDescriptor(es.subspan(offset), 4);
  if (Number<std::uint8_t>(config) != 0x40) Fail(Unsupported, "MP4 audio object is not MPEG-4 AAC");
  Slice(config, 0, 13);
  const auto asc = AacLcInitialization(FindDescriptor(config.subspan(13), 5));
  if (asc.size() > 65536) Fail(ContainerErrorCode::LimitExceeded, "AAC configuration exceeds 64 KiB");
  return Bytes(asc.begin(), asc.end());
}

VideoRectangle Aperture(ByteView clap, std::uint32_t width, std::uint32_t height) {
  const auto crop_width = Number<std::uint32_t>(clap);
  const auto crop_height = Number<std::uint32_t>(clap, 8);
  if (Number<std::uint32_t>(clap, 4) != 1 || Number<std::uint32_t>(clap, 12) != 1 ||
      !crop_width || !crop_height || crop_width > width || crop_height > height)
    Fail(Unsupported, "Unsupported MP4 clean aperture dimensions");
  auto coordinate = [&](std::uint32_t margin, std::size_t position) {
    const auto offset = Number<std::int32_t>(clap, position);
    const auto denominator = Number<std::uint32_t>(clap, position + 4);
    const auto top = static_cast<std::int64_t>(margin) * denominator + static_cast<std::int64_t>(offset) * 2;
    const auto bottom = static_cast<std::int64_t>(denominator) * 2;
    if (!bottom || top < 0 || top % bottom || top / bottom > margin)
      Fail(Unsupported, "Unsupported MP4 clean aperture offset");
    return static_cast<std::uint32_t>(top / bottom);
  };
  return {coordinate(width - crop_width, 16), coordinate(height - crop_height, 24), crop_width, crop_height};
}

VideoRotation Rotation(ByteView box, std::size_t matrix) {
  constexpr std::int32_t unit = 0x10000;
  const auto a = Number<std::int32_t>(box, matrix);
  const auto b = Number<std::int32_t>(box, matrix + 4);
  const auto c = Number<std::int32_t>(box, matrix + 12);
  const auto d = Number<std::int32_t>(box, matrix + 16);
  if (Number<std::uint32_t>(box, matrix + 8) || Number<std::uint32_t>(box, matrix + 20) ||
      Number<std::uint32_t>(box, matrix + 32) != 0x40000000u)
    Fail(Unsupported, "Perspective movie or track transform is unsupported");
  // Placement translations do not change the orientation of the visible image.
  if (a == unit && !b && !c && d == unit) return VideoRotation::None;
  if (!a && b == unit && c == -unit && !d) return VideoRotation::Clockwise90;
  if (a == -unit && !b && !c && d == -unit) return VideoRotation::Clockwise180;
  if (!a && b == -unit && c == unit && !d) return VideoRotation::Clockwise270;
  Fail(Unsupported, "Movie or track transform is not a right-angle rotation");
}

std::string EntryName(std::uint32_t tag) {
  std::string name(4, ' ');
  for (std::size_t i = 0; i < name.size(); ++i) name[i] = static_cast<char>((tag >> (24 - 8 * i)) & 255);
  return name;
}

CodecId PcmType(unsigned bits, bool big, bool floating, bool signed_integer) {
  if (floating && !signed_integer && bits == 32) return big ? CodecId::PcmF32Be : CodecId::PcmF32Le;
  if (!floating) {
    if (!signed_integer && bits == 8) return CodecId::PcmU8;
    if (signed_integer) {
      if (bits == 16) return big ? CodecId::PcmS16Be : CodecId::PcmS16Le;
      if (bits == 24) return big ? CodecId::PcmS24Be : CodecId::PcmS24Le;
      if (bits == 32) return big ? CodecId::PcmS32Be : CodecId::PcmS32Le;
    }
  }
  Fail(Unsupported, "Unsupported QuickTime PCM sample type");
}

void ReadPcmDescription(const BoxView& entry, MediaTrack& track) {
  const auto version = Number<std::uint16_t>(entry.bytes, 8);
  std::uint32_t channels = 0, rate = 0, declared_frame_bytes = 0;
  if (entry.tag == Tag("lpcm") && version == 2) {
    const auto size = Number<std::uint32_t>(entry.bytes, 28);
    const auto hz = std::bit_cast<double>(Number<std::uint64_t>(entry.bytes, 32));
    if (size < 72 || size - 8 > entry.bytes.size() || !std::isfinite(hz) || hz < 1 ||
        hz > std::numeric_limits<std::uint32_t>::max() || std::trunc(hz) != hz)
      Fail(Unsupported, "Unsupported QuickTime LPCM sample description");
    rate = static_cast<std::uint32_t>(hz);
    channels = Number<std::uint32_t>(entry.bytes, 40);
    const auto bits = Number<std::uint32_t>(entry.bytes, 48);
    const auto flags = Number<std::uint32_t>(entry.bytes, 52);
    declared_frame_bytes = Number<std::uint32_t>(entry.bytes, 56);
    if ((flags & ~15u) || !(flags & 8) || Number<std::uint32_t>(entry.bytes, 60) != 1)
      Fail(Unsupported, "QuickTime LPCM must be packed and interleaved with one frame per sample");
    track.codec = PcmType(bits, flags & 2, flags & 1, flags & 4);
  } else {
    if (version > 1) Fail(Unsupported, "Unsupported QuickTime PCM sound description version");
    const auto fixed_rate = Number<std::uint32_t>(entry.bytes, 24);
    if (fixed_rate & 0xffff) Fail(Unsupported, "Fractional QuickTime PCM sample rate");
    rate = fixed_rate >> 16;
    channels = Number<std::uint16_t>(entry.bytes, 16);
    if (version) {
      if (Number<std::uint32_t>(entry.bytes, 28) != 1)
        Fail(Unsupported, "QuickTime PCM must have one frame per sample");
      declared_frame_bytes = Number<std::uint32_t>(entry.bytes, 36);
    }
    switch (entry.tag) {
      case Tag("raw "): track.codec = CodecId::PcmU8; break;
      case Tag("sowt"): track.codec = CodecId::PcmS16Le; break;
      case Tag("twos"): track.codec = CodecId::PcmS16Be; break;
      case Tag("in24"):
      case Tag("in32"):
      case Tag("fl32"): {
        const auto offset = version ? 44u : 28u;
        Slice(entry.bytes, 0, offset);
        const auto extensions = entry.bytes.subspan(offset);
        auto endian = Child(extensions, "enda", false);
        if (endian.empty()) endian = Child(Child(extensions, "wave", false), "enda", false);
        const auto little = endian.empty() ? 0 : Number<std::uint16_t>(endian);
        if (little > 1) Fail(Invalid, "Invalid QuickTime PCM endianness");
        const auto floating = entry.tag == Tag("fl32");
        track.codec = PcmType(entry.tag == Tag("in24") ? 24 : 32, !little, floating, !floating);
        break;
      }
      default: Fail(Unsupported, "Unsupported QuickTime audio sample entry");
    }
    if ((entry.tag == Tag("raw ") || entry.tag == Tag("sowt") || entry.tag == Tag("twos")) &&
        Number<std::uint16_t>(entry.bytes, 18) != MovPcmBits(track.codec))
      Fail(Unsupported, "QuickTime PCM sample width does not match its entry");
  }
  const auto frame_bytes = (MovPcmBits(track.codec) / 8) * channels;
  if (!rate || !channels || channels > 64 || (version && frame_bytes != declared_frame_bytes))
    Fail(Invalid, "Inconsistent QuickTime PCM channels, rate, or frame size");
  track.kind = TrackKind::Audio;
  track.sample_rate = rate;
  track.channel_count = channels;
}

struct EditList {
  std::int64_t empty = 0;
  std::optional<std::int64_t> media_start;
  std::int64_t duration = 0;
};

std::int64_t SignedTicks(std::uint64_t value) {
  if (value > std::numeric_limits<std::int64_t>::max())
    Fail(ContainerErrorCode::LimitExceeded, "MP4 timestamp exceeds the signed timeline");
  return static_cast<std::int64_t>(value);
}

EditList ParseEdits(ByteView edit) {
  if (edit.empty()) return {};
  const auto list = Child(edit, "elst");
  CheckVersion(list, 1);
  const bool wide = Version(list) == 1;
  const auto stride = wide ? 20u : 12u;
  const auto count = Entries(list, stride);
  if (count > 2) Fail(Unsupported, "MP4 supports one presentation edit and an optional leading empty edit");
  EditList result;
  for (std::size_t i = 0; i < count; ++i) {
    const auto pos = 8 + i * stride;
    const auto length = wide ? SignedTicks(Number<std::uint64_t>(list, pos)) : Number<std::uint32_t>(list, pos);
    const auto start = wide ? Number<std::int64_t>(list, pos + 8) : Number<std::int32_t>(list, pos + 4);
    if (Number<std::uint32_t>(list, pos + stride - 4) != 0x10000)
      Fail(Unsupported, "MP4 edit playback rate is not one");
    if (start == -1 && count == 2 && i == 0) {
      result.empty = length;
    } else if (start >= 0 && i + 1 == count) {
      result.media_start = start;
      result.duration = length;
    } else {
      Fail(Unsupported, "Unsupported MP4 edit sequence");
    }
  }
  return result;
}

struct TrackIndex {
  std::uint32_t scale = 0;
  std::uint32_t clock = 0;
  std::int64_t shift = 0;
  unsigned nal_length = 0;
  std::vector<Mp4Sample> samples;
  std::vector<std::uint32_t> sync;
  std::uint32_t default_duration = 0;
  std::uint32_t default_size = 0;
  std::uint32_t default_flags = 0;
  std::optional<std::int64_t> next_dts;
  bool defaults = false;

  MediaTimestamp Time(std::int64_t ticks) const {
    return {AddTime(ExactTicks({ticks, scale}, clock), shift), clock};
  }
};

template<class Visit> void VisitChunks(ByteView stbl, std::uint32_t count, Visit visit) {
  const auto offsets64 = Child(stbl, "co64", false);
  const auto offsets = offsets64.empty() ? Child(stbl, "stco") : offsets64;
  const auto stride = offsets64.empty() ? 4 : 8;
  const auto chunk_count = Entries(offsets, stride);
  const auto mapping = Child(stbl, "stsc");
  const auto mappings = Entries(mapping, 12);
  std::uint32_t index = 0;
  std::size_t entry = 0;
  std::uint32_t per_chunk = 0;
  std::uint64_t previous_end = 0;
  for (std::size_t chunk = 1; chunk <= chunk_count; ++chunk) {
    const auto position = 8 + entry * 12;
    if (entry < mappings && Number<std::uint32_t>(mapping, position) == chunk) {
      per_chunk = Number<std::uint32_t>(mapping, position + 4);
      if (Number<std::uint32_t>(mapping, position + 8) != 1)
        Fail(Unsupported, "MP4 track uses more than one sample description");
      ++entry;
    }
    if (!per_chunk || per_chunk > count - index) Fail(Invalid, "Invalid MP4 samples per chunk");
    auto offset = stride == 4 ? Number<std::uint32_t>(offsets, 8 + (chunk - 1) * stride)
                             : Number<std::uint64_t>(offsets, 8 + (chunk - 1) * stride);
    if (offset < previous_end) Fail(Unsupported, "MP4 track chunks are not stored in decode order");
    previous_end = visit(index, per_chunk, offset);
    index += per_chunk;
  }
  if (entry != mappings || index != count) Fail(Invalid, "MP4 chunk table does not cover all samples");
}

void ParsePcmSamples(ByteView stbl, TrackIndex& index, const MediaTrack& track, std::size_t remaining) {
  const auto frame_bytes = (MovPcmBits(track.codec) / 8) * *track.channel_count;
  const auto rate = *track.sample_rate;
  if (index.scale % rate) Fail(Unsupported, "QuickTime PCM track clock is not an integer multiple of its sample rate");
  const auto ticks = index.scale / rate;
  const auto sizes = Child(stbl, "stsz");
  const auto size = Number<std::uint32_t>(sizes, 4);
  const auto count = Number<std::uint32_t>(sizes, 8);
  if (size != frame_bytes) Fail(Unsupported, "QuickTime PCM requires a constant size per sample frame");
  if (!Child(stbl, "ctts", false).empty() || !Child(stbl, "stss", false).empty())
    Fail(Unsupported, "QuickTime PCM composition and sync tables are unsupported");

  const auto timing = Child(stbl, "stts");
  std::uint32_t covered = 0;
  for (std::size_t i = 0, entries = Entries(timing, 8); i < entries; ++i) {
    const auto run = Number<std::uint32_t>(timing, 8 + i * 8);
    if (!run || run > count - covered || Number<std::uint32_t>(timing, 12 + i * 8) != ticks)
      Fail(Invalid, "Inconsistent QuickTime PCM sample timing");
    covered += run;
  }
  if (covered != count) Fail(Invalid, "QuickTime PCM timing does not cover all frames");

  const auto max_frames = std::min(16384u / frame_bytes, 0xffffffffu / ticks);
  std::int64_t time = 0;
  // A sample table can describe hours of one-frame samples. Index bounded blocks within each chunk instead.
  VisitChunks(stbl, count, [&](std::uint32_t, std::uint32_t frames, std::uint64_t offset) {
    while (frames) {
      if (index.samples.size() == remaining) Fail(ContainerErrorCode::LimitExceeded, "QuickTime PCM index exceeds its limit");
      const auto block = std::min(frames, max_frames);
      const auto bytes = block * frame_bytes;
      const auto duration = block * ticks;
      index.sync.push_back(static_cast<std::uint32_t>(index.samples.size()));
      index.samples.push_back({offset, bytes, time, time, duration, true});
      time = AddTime(time, duration);
      offset = AddOffset(offset, bytes);
      frames -= block;
    }
    return offset;
  });
}

void ParseSamples(ByteView stbl, TrackIndex& track, std::size_t remaining) {
  const auto sizes = Child(stbl, "stsz");
  const auto uniform = Number<std::uint32_t>(sizes, 4);
  const auto count = Number<std::uint32_t>(sizes, 8);
  if (count > remaining) Fail(ContainerErrorCode::LimitExceeded, "MP4 index exceeds one million samples");
  if (!uniform) Slice(sizes, 12, static_cast<std::size_t>(count) * 4);
  track.samples.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto& sample = track.samples[i];
    sample.size = uniform ? uniform : Number<std::uint32_t>(sizes, 12 + i * 4);
    if (!sample.size || sample.size > MaxContainerPacket)
      Fail(ContainerErrorCode::LimitExceeded, "MP4 sample is empty or exceeds 16 MiB");
  }

  const auto timing = Child(stbl, "stts");
  std::int64_t dts = 0;
  std::size_t index = 0;
  for (std::size_t i = 0, entries = Entries(timing, 8); i < entries; ++i) {
    const auto run = Number<std::uint32_t>(timing, 8 + i * 8);
    const auto duration = Number<std::uint32_t>(timing, 12 + i * 8);
    if (!run || run > count - index || !duration) Fail(Invalid, "Invalid MP4 decoding time run");
    for (std::uint32_t j = 0; j < run; ++j) {
      auto& sample = track.samples[index++];
      sample.dts = sample.pts = dts;
      sample.duration = duration;
      dts = AddTime(dts, duration);
    }
  }
  if (index != count) Fail(Invalid, "MP4 decoding time table does not cover all samples");

  const auto composition = Child(stbl, "ctts", false);
  if (!composition.empty()) {
    CheckVersion(composition, 1);
    index = 0;
    for (std::size_t i = 0, entries = Entries(composition, 8); i < entries; ++i) {
      const auto run = Number<std::uint32_t>(composition, 8 + i * 8);
      const auto position = 12 + i * 8;
      const std::int64_t offset = Version(composition)
          ? static_cast<std::int64_t>(Number<std::int32_t>(composition, position))
          : Number<std::uint32_t>(composition, position);
      if (!run || run > count - index) Fail(Invalid, "Invalid MP4 composition time run");
      for (std::uint32_t j = 0; j < run; ++j) {
        auto& sample = track.samples[index++];
        sample.pts = AddTime(sample.dts, offset);
      }
    }
    if (index != count) Fail(Invalid, "MP4 composition time table does not cover all samples");
  }

  VisitChunks(stbl, count, [&](std::uint32_t first, std::uint32_t per_chunk, std::uint64_t offset) {
    for (std::uint32_t i = 0; i < per_chunk; ++i) {
      auto& sample = track.samples[first + i];
      sample.offset = offset;
      offset = AddOffset(offset, sample.size);
    }
    return offset;
  });

  const auto sync = Child(stbl, "stss", false);
  if (sync.empty()) {
    for (auto& sample : track.samples) sample.sync = true;
  } else {
    std::uint32_t previous = 0;
    for (std::size_t i = 0, entries = Entries(sync, 4); i < entries; ++i) {
      const auto number = Number<std::uint32_t>(sync, 8 + i * 4);
      if (number <= previous || number > count) Fail(Invalid, "Invalid MP4 sync sample index");
      track.samples[number - 1].sync = true;
      previous = number;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (track.samples[i].sync) track.sync.push_back(i);
  }
  std::stable_sort(track.sync.begin(), track.sync.end(), [&](auto first, auto second) {
    return track.samples[first].pts < track.samples[second].pts;
  });
}

struct SampleRef {
  std::uint32_t track;
  std::uint32_t index;
};

class Mp4Reader final : public ContainerReader {
public:
  Mp4Reader(ContainerInput input, std::optional<ContainerFormat> hint)
      : ContainerReader(std::move(input), ContainerFormat::Mp4) {
    bool movie = false, branded = false;
    std::size_t boxes = 0;
    while (const auto box = ReadBox(this->input)) {
      if (++boxes > 65536 || (!this->input.Random() && box->offset > MaxContainerMetadata))
        Fail(ContainerErrorCode::LimitExceeded, "MP4 movie discovery exceeds its budget");
      if (box->tag == Tag("ftyp")) {
        if (branded || movie || box->size < 8) Fail(Invalid, "Invalid or misplaced file type box");
        const auto major = Number<std::uint32_t>(this->input.ReadBytes(4));
        if (major == Tag("qt  ")) format = ContainerFormat::Mov;
        if ((!this->input.Random() || !this->input.Size()) && box->size > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "File type box exceeds probe budget");
        this->input.Skip(box->size - 4);
        branded = true;
      } else if (box->tag == Tag("moov")) {
        if (movie) Fail(Invalid, "Duplicate MP4 movie");
        if (hint && *hint != format) Fail(ContainerErrorCode::UnrecognizedFormat, "Movie brand does not match the format hint");
        Movie(ReadMetadata(this->input, box->size));
        movie = true;
        if (!this->input.Random() || layout == ContainerLayout::Fragmented) break;
      } else if (box->tag == Tag("mdat")) {
        if (!this->input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "MP4 media data precedes movie metadata");
        media_.emplace_back(this->input.Position(), AddOffset(this->input.Position(), box->size));
        if (box->to_eof) break;
        this->input.Seek(media_.back().second);
      } else {
        if ((!this->input.Random() || !this->input.Size()) && box->size > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "MP4 probe skip exceeds its budget");
        this->input.Skip(box->size);
      }
    }
    if (!movie) Fail(Invalid, "Missing MP4 movie metadata");
    if (layout == ContainerLayout::Default) {
      OrderSamples();
      if (this->input.Random()) {
        std::size_t range = 0;
        for (auto ref : order_) {
          const auto& sample = Sample(ref);
          while (range < media_.size() && sample.offset >= media_[range].second) ++range;
          if (range == media_.size() || sample.offset < media_[range].first ||
              AddOffset(sample.offset, sample.size) > media_[range].second)
            Fail(Invalid, "MP4 sample lies outside media data");
        }
      }
    }
  }

  std::optional<DemuxItem> Read() override {
    if (cursor_ == order_.size()) {
      if (layout == ContainerLayout::Default || !NextFragment()) return {};
    }
    const auto ref = order_[cursor_++];
    const auto& sample = Sample(ref);
    if (input.Random() && layout == ContainerLayout::Default) {
      input.Seek(sample.offset);
    } else {
      PositionSample(sample);
    }
    const auto& track = indexes_[ref.track];
    auto data = Own(input.ReadBytes(sample.size));
    const auto pts = track.Time(sample.pts), dts = track.Time(sample.dts);
    return ContainerPacket{tracks[ref.track].id, std::move(data), pts, dts,
                           MediaTimestamp{sample.duration, track.scale}, sample.sync};
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override {
    if (seek && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "MP4 input cannot seek");
    if (layout == ContainerLayout::Fragmented)
      Fail(ContainerErrorCode::IndexUnavailable, "Fragmented MP4 has no covered seek index");
    const auto index = PacketTrack(tracks, id);
    const auto& track = indexes_[index];
    const auto& points = track.sync;
    auto at = points.end();
    if (direction == SeekDirection::AtOrAfter) {
      at = std::lower_bound(points.begin(), points.end(), time, [&](std::uint32_t sample, MediaTimestamp target) {
        return track.Time(track.samples[sample].pts).Compare(target) < 0;
      });
      if (at == points.end()) return {};
    } else {
      at = std::upper_bound(points.begin(), points.end(), time, [&](MediaTimestamp target, std::uint32_t sample) {
        return target.Compare(track.Time(track.samples[sample].pts)) < 0;
      });
      if (at == points.begin()) return {};
      --at;
    }
    const auto& sample = track.samples[*at];
    auto kind = track.nal_length ? SeekPoint::Kind::SyncMarked : SeekPoint::Kind::Independent;
    const auto& description = tracks[index];
    const bool avc = description.codec == CodecId::H264;
    const bool hvc1 = description.codec == CodecId::Hevc && description.sample_entry == "hvc1";
    if ((avc || hvc1) && input.Random()) {
      // Inspect the sync sample without changing the session's observable read position.
      const auto previous = input.Position();
      input.Seek(sample.offset);
      try {
        const auto bytes = input.ReadBytes(sample.size);
        const bool idr = avc ? AvcIdr(bytes, track.nal_length, Invalid)
                             : HevcIdr(bytes, track.nal_length, true, Invalid);
        if (idr) kind = SeekPoint::Kind::Independent;
      } catch (...) {
        input.Seek(previous);
        throw;
      }
      input.Seek(previous);
    }
    if (seek) {
      const auto entry = std::lower_bound(order_.begin(), order_.end(), sample.offset,
                                          [&](SampleRef ref, std::uint64_t offset) { return Sample(ref).offset < offset; });
      cursor_ = static_cast<std::size_t>(entry - order_.begin());
    }
    return SeekPoint{id, track.Time(sample.pts), kind, sample.offset, {}};
  }

private:
  const Mp4Sample& Sample(SampleRef ref) const { return indexes_[ref.track].samples[ref.index]; }

  void OrderSamples() {
    order_.clear();
    cursor_ = 0;
    for (std::uint32_t i = 0; i < indexes_.size(); ++i) {
      for (std::uint32_t j = 0; j < indexes_[i].samples.size(); ++j) order_.push_back({i, j});
    }
    std::ranges::sort(order_, {}, [&](SampleRef ref) { return Sample(ref).offset; });
    std::uint64_t end = 0;
    for (const auto ref : order_) {
      const auto& sample = Sample(ref);
      if (sample.offset < end) Fail(Invalid, "MP4 sample byte ranges overlap");
      end = AddOffset(sample.offset, sample.size);
    }
  }

  void Movie(ByteView movie) {
    const auto header = Child(movie, "mvhd");
    CheckVersion(header, 1);
    const auto wide = Version(header) == 1;
    movie_scale_ = Number<std::uint32_t>(header, wide ? 20 : 12);
    const auto length = wide ? Number<std::uint64_t>(header, 24) : Number<std::uint32_t>(header, 16);
    if (!movie_scale_) Fail(Invalid, "Zero MP4 movie timescale");
    movie_rotation_ = Rotation(header, wide ? 48 : 36);
    if (length && length <= std::numeric_limits<std::int64_t>::max() && (wide || length != 0xffffffffu))
      duration = MediaTimestamp{static_cast<std::int64_t>(length), movie_scale_};
    const auto extensions = Child(movie, "mvex", false);
    if (!extensions.empty()) {
      if (format == ContainerFormat::Mov) Fail(Unsupported, "Fragmented QuickTime movies are unsupported");
      layout = ContainerLayout::Fragmented;
    }
    std::size_t count = 0;
    while (!movie.empty()) {
      const auto box = NextBox(movie);
      if (box.tag == Tag("trak")) ReadTrack(box.bytes, count);
    }
    if (tracks.empty()) Fail(Invalid, "Movie has no audio or video tracks");
    if (layout == ContainerLayout::Fragmented) {
      if (count) Fail(Unsupported, "MP4 hybrid sample tables and fragments are unsupported");
      ReadDefaults(extensions);
    }
  }

  void ReadTrack(ByteView box, std::size_t& count) {
    const auto mdia = Child(box, "mdia");
    const auto handler = Number<std::uint32_t>(Child(mdia, "hdlr"), 8);
    // QuickTime files may have independent timecode or timed metadata tracks beside the editing media.
    if (format == ContainerFormat::Mov &&
        (handler == Tag("tmcd") || handler == Tag("meta") || handler == Tag("mdta"))) return;
    if (tracks.size() == MaxContainerTracks)
      Fail(ContainerErrorCode::LimitExceeded, "Too many movie tracks");
    MediaTrack track;
    TrackIndex index;
    const auto tkhd = Child(box, "tkhd");
    CheckVersion(tkhd, 1);
    track.id = Number<std::uint32_t>(tkhd, Version(tkhd) ? 20 : 12);
    if (!track.id || std::ranges::find(tracks, track.id, &MediaTrack::id) != tracks.end())
      Fail(Invalid, "MP4 track IDs must be nonzero and unique");
    const auto mdhd = Child(mdia, "mdhd");
    CheckVersion(mdhd, 1);
    index.clock = index.scale = Number<std::uint32_t>(mdhd, Version(mdhd) ? 20 : 12);
    if (!index.scale) Fail(Invalid, "Zero MP4 track timescale");
    track.timescale = index.scale;
    const auto minf = Child(mdia, "minf");
    const auto references = Child(Child(minf, "dinf"), "dref");
    if (Number<std::uint32_t>(references, 4) != 1)
      Fail(Unsupported, "MP4 external or multiple data references are unsupported");
    auto urls = references.subspan(8);
    const auto url = NextBox(urls);
    if ((url.tag != Tag("url ") && !(format == ContainerFormat::Mov && url.tag == Tag("alis"))) ||
        Number<std::uint32_t>(url.bytes) != 1 || !urls.empty())
      Fail(Unsupported, "MP4 external data reference");
    const auto stbl = Child(minf, "stbl");
    const auto description = Child(stbl, "stsd");
    if (Number<std::uint32_t>(description, 4) != 1)
      Fail(Unsupported, "MP4 requires a single sample description per track");
    auto entries = description.subspan(8);
    const auto entry = NextBox(entries);
    if (Number<std::uint16_t>(entry.bytes, 6) != 1 || !entries.empty())
      Fail(Unsupported, "Unsupported MP4 sample description reference");
    if (handler == Tag("soun") && entry.tag == Tag("mp4a")) {
      const auto version = Number<std::uint16_t>(entry.bytes, 8);
      if (version > 1 || (version && format != ContainerFormat::Mov))
        Fail(Unsupported, "Unsupported AAC audio sample entry version");
      const auto base = version ? 44u : 28u;
      Slice(entry.bytes, 0, base);
      const auto extensions = entry.bytes.subspan(base);
      auto descriptor = Child(extensions, "esds", !version);
      if (descriptor.empty()) descriptor = Child(Child(extensions, "wave"), "esds");
      track.initialization_data = AacInitialization(descriptor);
      auto config = ParseAac(track.initialization_data);
      if (!config.Succeeded()) Fail(Unsupported, config.Error().message);
      track.kind = TrackKind::Audio;
      track.codec = CodecId::Aac;
      track.sample_rate = config.Value().sample_rate;
      track.channel_count = config.Value().channels;
    } else if (handler == Tag("vide")) {
      track.kind = TrackKind::Video;
      const auto width = Number<std::uint16_t>(entry.bytes, 24), height = Number<std::uint16_t>(entry.bytes, 26);
      if (!width || !height) Fail(Invalid, "Invalid MP4 visual sample dimensions");
      track.coded_width = width;
      track.coded_height = height;
      Slice(entry.bytes, 0, 78);
      const auto extensions = entry.bytes.subspan(78);
      ByteView config;
      if (entry.tag == Tag("avc1")) {
        track.codec = CodecId::H264;
        config = Child(extensions, "avcC");
        const auto avc = ParseAvc(config);
        if (!avc.Succeeded()) Fail(Unsupported, avc.Error().message);
        index.nal_length = avc.Value().length_size;
      } else if (entry.tag == Tag("hvc1") || entry.tag == Tag("hev1")) {
        track.codec = CodecId::Hevc;
        track.sample_entry = EntryName(entry.tag);
        config = Child(extensions, "hvcC");
        index.nal_length = HevcNalLength(config, entry.tag == Tag("hvc1"), Invalid);
      } else if (format == ContainerFormat::Mov && ProResEntry(entry.tag)) {
        track.codec = CodecId::ProRes;
        track.sample_entry = EntryName(entry.tag);
      } else {
        Fail(Unsupported, "Unsupported MP4 or QuickTime video sample entry");
      }
      if (index.nal_length) {
        track.framing = PacketFraming::LengthPrefixedNals;
        track.initialization_data.assign(config.begin(), config.end());
      }
      const auto rotation = Rotation(tkhd, Version(tkhd) ? 52 : 40);
      track.rotation = static_cast<VideoRotation>(
          (static_cast<unsigned>(movie_rotation_) + static_cast<unsigned>(rotation)) % 4);
      const auto clap = Child(extensions, "clap", false);
      if (!clap.empty()) track.visible = Aperture(clap, width, height);
    } else if (handler == Tag("soun") && format == ContainerFormat::Mov) {
      ReadPcmDescription(entry, track);
    } else {
      Fail(Unsupported, "Unsupported MP4 or QuickTime track");
    }
    if (MovPcmBits(track.codec)) {
      ParsePcmSamples(stbl, index, track, MaxContainerSamples - count);
    } else {
      ParseSamples(stbl, index, MaxContainerSamples - count);
    }
    ApplyEdits(track, index, ParseEdits(Child(box, "edts", false)));
    count += index.samples.size();
    tracks.push_back(std::move(track));
    indexes_.push_back(std::move(index));
  }

  void ApplyEdits(MediaTrack& track, TrackIndex& index, const EditList& edit) {
    if (edit.media_start) {
      // Only introduce a common scale if the movie edit cannot be represented on the track clock.
      const MediaTimestamp leading{edit.empty, movie_scale_};
      const auto track_ticks = leading.Rescale(index.scale);
      if (!track_ticks || *track_ticks != leading) index.clock = CommonScale(movie_scale_, index.scale);
      index.shift = SubTime(ExactTicks(leading, index.clock), ExactTicks({*edit.media_start, index.scale}, index.clock));
      track.timescale = index.clock;
    }
    if (index.samples.empty()) return;
    const auto first = std::ranges::min_element(index.samples, {}, &Mp4Sample::pts)->pts;
    const auto visible = edit.media_start ? MediaTimestamp{edit.empty, movie_scale_} : index.Time(first);
    if (!start || visible.Compare(*start) < 0) start = visible;
    if (!track.sample_rate || !edit.media_start) return;
    const auto rate = *track.sample_rate;
    auto trim = [&](MediaTimestamp time) -> std::optional<std::uint64_t> {
      const auto frames = time.Rescale(rate);
      if (frames && *frames == time && frames->ticks >= 0) return static_cast<std::uint64_t>(frames->ticks);
      return {};
    };
    track.priming_samples = trim({SubTime(*edit.media_start, first), index.scale});
    if (edit.duration) {
      std::int64_t last = first;
      for (auto& sample : index.samples) last = std::max(last, AddTime(sample.pts, sample.duration));
      auto scale = CommonScale(movie_scale_, index.scale);
      auto tail = SubTime(ExactTicks({SubTime(last, *edit.media_start), index.scale}, scale),
                          ExactTicks({edit.duration, movie_scale_}, scale));
      track.padding_samples = trim({tail, scale});
    }
  }

  void ReadDefaults(ByteView boxes) {
    while (!boxes.empty()) {
      const auto box = NextBox(boxes);
      if (box.tag != Tag("trex")) continue;
      CheckVersion(box.bytes, 0);
      const auto id = Number<std::uint32_t>(box.bytes, 4);
      auto& track = indexes_[FragmentTrack(id)];
      if (track.defaults || Number<std::uint32_t>(box.bytes, 8) != 1)
        Fail(Unsupported, "Unsupported MP4 fragment default description");
      track.default_duration = Number<std::uint32_t>(box.bytes, 12);
      track.default_size = Number<std::uint32_t>(box.bytes, 16);
      track.default_flags = Number<std::uint32_t>(box.bytes, 20);
      track.defaults = true;
    }
    if (std::ranges::any_of(indexes_, [](const auto& track) { return !track.defaults; }))
      Fail(Invalid, "MP4 fragment defaults are missing for a track");
  }

  std::size_t FragmentTrack(std::uint32_t id) const {
    const auto track = std::ranges::find(tracks, id, &MediaTrack::id);
    if (track == tracks.end()) Fail(Invalid, "MP4 fragment references an unknown track");
    return static_cast<std::size_t>(track - tracks.begin());
  }

  bool NextFragment() {
    // A size-zero mdat consumes the rest of the input. After its declared samples there is no next box.
    if (media_to_eof_) return false;
    if (media_end_ > input.Position()) input.Skip(media_end_ - input.Position());
    media_end_ = 0;
    std::size_t boxes = 0;
    while (const auto box = ReadBox(input)) {
      if (++boxes > 4096) Fail(ContainerErrorCode::LimitExceeded, "Too many boxes between MP4 fragments");
      if (box->tag == Tag("moof")) {
        for (auto& index : indexes_) index.samples.clear();
        Fragment(ReadMetadata(input, box->size), box->offset);
        OrderSamples();
        if (!order_.empty()) return true;
      } else {
        if (box->to_eof) return false;
        if ((!input.Random() || !input.Size()) && box->size > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "MP4 fragment header skip exceeds its budget");
        input.Skip(box->size);
      }
    }
    return false;
  }

  void Fragment(ByteView moof, std::uint64_t offset) {
    std::size_t count = 0;
    auto previous_end = offset;
    while (!moof.empty()) {
      const auto box = NextBox(moof);
      if (box.tag != Tag("traf")) continue;
      const auto tfhd = Child(box.bytes, "tfhd");
      CheckVersion(tfhd, 0);
      const auto flags = Number<std::uint32_t>(tfhd);
      if (flags & ~0x03003bu) Fail(Unsupported, "Unsupported MP4 track fragment flags");
      auto& track = indexes_[FragmentTrack(Number<std::uint32_t>(tfhd, 4))];
      std::size_t pos = 8;
      auto base = flags & 0x20000 ? offset : previous_end;
      if (flags & 1) {
        base = Number<std::uint64_t>(tfhd, pos);
        pos += 8;
      }
      auto field = [&](unsigned flag, std::uint32_t fallback) {
        if (!(flags & flag)) return fallback;
        const auto value = Number<std::uint32_t>(tfhd, pos);
        pos += 4;
        return value;
      };
      if (field(2, 1) != 1) Fail(Unsupported, "MP4 fragment changes sample description");
      const auto length = field(8, track.default_duration);
      const auto size = field(16, track.default_size);
      const auto sample_flags = field(32, track.default_flags);
      const auto tfdt = Child(box.bytes, "tfdt", false);
      auto dts = track.next_dts.value_or(0);
      if (!tfdt.empty()) {
        CheckVersion(tfdt, 1);
        dts = Version(tfdt) ? SignedTicks(Number<std::uint64_t>(tfdt, 4)) : Number<std::uint32_t>(tfdt, 4);
      }
      if (track.next_dts && dts < *track.next_dts)
        Fail(Unsupported, "MP4 fragment decode times move backwards");
      auto run_offset = base;
      auto children = box.bytes;
      while (!children.empty()) {
        const auto child = NextBox(children);
        if (child.tag != Tag("trun")) continue;
        ParseRun(child.bytes, track, base, run_offset, dts, length, size, sample_flags, count);
      }
      track.next_dts = dts;
      previous_end = run_offset;
    }
  }

  void ParseRun(ByteView run, TrackIndex& track, std::uint64_t base, std::uint64_t& offset,
                std::int64_t& dts, std::uint32_t duration, std::uint32_t size, std::uint32_t sample_flags,
                std::size_t& total) {
    CheckVersion(run, 1);
    const auto flags = Number<std::uint32_t>(run);
    if ((flags & ~0x01000f05u) || (flags & 0x404u) == 0x404u)
      Fail(Unsupported, "Unsupported MP4 sample run flags");
    const auto count = Number<std::uint32_t>(run, 4);
    if (count > MaxContainerSamples - total)
      Fail(ContainerErrorCode::LimitExceeded, "MP4 fragment exceeds its sample limit");
    total += count;
    std::size_t pos = 8;
    if (flags & 1) {
      const std::int64_t delta = Number<std::int32_t>(run, pos);
      if (delta < 0 && static_cast<std::uint64_t>(-delta) > base) Fail(Invalid, "Invalid MP4 run offset");
      offset = delta < 0 ? base - static_cast<std::uint64_t>(-delta) : AddOffset(base, delta);
      pos += 4;
    }
    auto field = [&](unsigned flag, std::uint32_t fallback) {
      if (!(flags & flag)) return fallback;
      const auto value = Number<std::uint32_t>(run, pos);
      pos += 4;
      return value;
    };
    const auto first_flags = field(4, sample_flags);
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto ticks = field(0x100, duration);
      const auto bytes = field(0x200, size);
      const auto attrs = field(0x400, i ? sample_flags : first_flags);
      const auto offset_bits = field(0x800, 0);
      const std::int64_t composition = Version(run) ? static_cast<std::int64_t>(std::bit_cast<std::int32_t>(offset_bits))
                                                    : offset_bits;
      if (!ticks || !bytes || bytes > MaxContainerPacket)
        Fail(Invalid, "MP4 fragment has invalid sample duration or size");
      if (!track.samples.empty() && offset < AddOffset(track.samples.back().offset, track.samples.back().size))
        Fail(Unsupported, "MP4 fragment samples are not stored in decode order");
      track.samples.push_back({offset, bytes, AddTime(dts, composition), dts, ticks, !(attrs & 0x10000)});
      dts = AddTime(dts, ticks);
      offset = AddOffset(offset, bytes);
    }
  }

  void PositionSample(const Mp4Sample& sample) {
    // The packet table supplies locations. Verify that each location lies inside an actual mdat.
    std::size_t boxes = 0;
    while (sample.offset >= media_end_) {
      if (media_end_ > input.Position()) input.Skip(media_end_ - input.Position());
      const auto box = ReadBox(input);
      if (!box || ++boxes > 4096 || box->tag == Tag("moof"))
        Fail(Invalid, "MP4 sample has no matching media data");
      if (box->tag == Tag("mdat")) {
        media_end_ = AddOffset(input.Position(), box->size);
        media_to_eof_ = box->to_eof;
      } else {
        if ((!input.Random() || !input.Size()) && box->size > MaxContainerMetadata)
          Fail(ContainerErrorCode::LimitExceeded, "MP4 media header skip exceeds its budget");
        input.Skip(box->size);
      }
    }
    if (sample.offset < input.Position() || AddOffset(sample.offset, sample.size) > media_end_)
      Fail(Invalid, "MP4 sample exceeds its media data box");
    input.Skip(sample.offset - input.Position());
  }

  std::vector<TrackIndex> indexes_;
  std::vector<SampleRef> order_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> media_;
  std::uint32_t movie_scale_ = 0;
  VideoRotation movie_rotation_ = VideoRotation::None;
  std::size_t cursor_ = 0;
  std::uint64_t media_end_ = 0;
  bool media_to_eof_ = false;
};
} // namespace

std::unique_ptr<ContainerReader> OpenMp4Reader(ContainerInput input, std::optional<ContainerFormat> hint) {
  return std::make_unique<Mp4Reader>(std::move(input), hint);
}
} // namespace huxerui::media::detail
