#include "mp4.h"
#include "framed_audio.h"

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

std::optional<ByteView> FindChild(ByteView input, std::string_view tag, bool mov_visual = false) {
  std::optional<ByteView> found;
  while (!input.empty()) {
    // QuickTime visual sample descriptions can end with a four-byte zero terminator.
    if (mov_visual && input.size() == 4 && Number<std::uint32_t>(input) == 0) break;
    const auto child = NextBox(input);
    if (child.tag != Tag(tag)) continue;
    if (found) Fail(Invalid, "Duplicate MP4 box: " + std::string(tag));
    found = child.bytes;
  }
  return found;
}

ByteView Child(ByteView input, std::string_view tag, bool required = true, bool mov_visual = false) {
  const auto found = FindChild(input, tag, mov_visual);
  if (!found && required) Fail(Invalid, "Missing MP4 box: " + std::string(tag));
  if (found && found->empty()) Fail(Invalid, "Empty MP4 box: " + std::string(tag));
  return found.value_or(ByteView{});
}

std::optional<MediaAudioMetadata> ReadAudioMetadata(ByteView movie) {
  const auto udta = FindChild(movie, "udta").value_or(ByteView{});
  const auto meta = FindChild(udta, "meta").value_or(ByteView{});
  if (meta.size() < 4 || Number<std::uint32_t>(meta) != 0) return {};
  const auto header = FindChild(meta.subspan(4), "hdlr").value_or(ByteView{});
  if (header.size() < 12 || Number<std::uint32_t>(header, 8) != Tag("mdir")) return {};
  auto items = FindChild(meta.subspan(4), "ilst").value_or(ByteView{});
  MediaAudioMetadata metadata;
  while (!items.empty()) {
    const auto item = NextBox(items);
    const auto text = std::ranges::find_if(Mp4AudioTags, [&](auto tag) { return Tag(tag.name) == item.tag; });
    if (text == std::end(Mp4AudioTags) && item.tag != Tag("trkn") && item.tag != Tag("covr")) continue;
    auto values = item.bytes;
    while (!values.empty()) {
      const auto value = NextBox(values);
      if (value.tag != Tag("data") || value.bytes.size() < 8) continue;
      const auto type = Number<std::uint32_t>(value.bytes);
      const auto data = value.bytes.subspan(8);
      if (text != std::end(Mp4AudioTags)) {
        auto& field = metadata.*text->field;
        if (field.empty() && type == 1 && !data.empty() && data.size() <= MaxAudioText &&
            std::ranges::find(data, std::byte{0}) == data.end())
          field.assign(reinterpret_cast<const char*>(data.data()), data.size());
      } else if (item.tag == Tag("trkn") && metadata.track_number.empty() && type == 0 && data.size() >= 6) {
        const auto number = Number<std::uint16_t>(data, 2), total = Number<std::uint16_t>(data, 4);
        if (number) {
          metadata.track_number = std::to_string(number);
          if (total) metadata.track_number += "/" + std::to_string(total);
        }
      } else if (item.tag == Tag("covr") && !metadata.cover && (type == 13 || type == 14) &&
                 !data.empty() && data.size() <= MaxContainerPacket - 64) {
        metadata.cover = MediaAudioMetadata::Cover{type == 13 ? "image/jpeg" : "image/png", Own(Bytes(data.begin(), data.end()))};
      }
    }
  }
  if (!HasAudioMetadata(metadata)) return {};
  return metadata;
}

struct TopBox {
  std::uint32_t tag;
  std::uint64_t offset;
  std::uint64_t size;
  bool to_eof;
};

std::optional<TopBox> ReadBox(ContainerInput& input, bool growing = false) {
  const auto offset = input.Position();
  const auto file_size = input.Size();
  if (file_size && offset > *file_size) Fail(Invalid, "MP4 box offset exceeds input size");
  if (growing && *file_size - offset < 8) return {};
  std::array<std::byte, 8> header;
  if (!input.ReadExact(header)) return {};
  const auto tag = Number<std::uint32_t>(header, 4);
  std::uint64_t size = Number<std::uint32_t>(header);
  const bool to_eof = !size && !file_size;
  if (size == 1) {
    if (growing && *file_size - offset < 16) return {};
    size = Number<std::uint64_t>(input.ReadBytes(8));
  } else if (!size) {
    if (growing) Fail(Unsupported, "Growing MP4 requires explicit box sizes");
    if (file_size) {
      size = *file_size - offset;
    } else if (tag == Tag("mdat")) {
      size = std::numeric_limits<std::uint64_t>::max() - offset;
    } else {
      Fail(Unsupported, "Unbounded MP4 box requires a known input size");
    }
  }
  const auto header_size = input.Position() - offset;
  if (size < header_size) Fail(Invalid, "MP4 box is shorter than its header");
  if (growing && size > *file_size - offset) return {};
  if (file_size && size > *file_size - offset)
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

void ReadPcmDescription(const BoxView& entry, MediaSampleDescription& track) {
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
  track.sample_rate = rate;
  track.channel_count = channels;
}

std::int64_t SignedTicks(std::uint64_t value) {
  if (value > std::numeric_limits<std::int64_t>::max())
    Fail(ContainerErrorCode::LimitExceeded, "MP4 timestamp exceeds the signed timeline");
  return static_cast<std::int64_t>(value);
}

std::vector<MediaTrack::Edit> ParseEdits(ByteView edit, std::uint32_t movie_scale, std::uint32_t media_scale) {
  if (edit.empty()) return {};
  const auto list = Child(edit, "elst");
  CheckVersion(list, 1);
  const bool wide = Version(list) == 1;
  const auto stride = wide ? 20u : 12u;
  const auto count = Entries(list, stride);
  if (count > MaxMp4Edits) Fail(ContainerErrorCode::LimitExceeded, "MP4 edit list exceeds its limit");
  std::vector<MediaTrack::Edit> edits;
  for (std::size_t i = 0; i < count; ++i) {
    const auto pos = 8 + i * stride;
    const auto length = wide ? SignedTicks(Number<std::uint64_t>(list, pos)) : Number<std::uint32_t>(list, pos);
    const auto start = wide ? Number<std::int64_t>(list, pos + 8) : Number<std::int32_t>(list, pos + 4);
    if (Number<std::uint32_t>(list, pos + stride - 4) != 0x10000)
      Fail(Unsupported, "MP4 edit playback rate is not one");
    if (start < -1) Fail(Invalid, "Invalid MP4 edit media position");
    edits.push_back({{length, movie_scale}, start == -1 ? std::nullopt : std::optional(MediaTimestamp{start, media_scale})});
  }
  return edits;
}

struct TrackIndex {
  std::uint32_t scale = 0;
  std::optional<Mp4EditTimeline> timeline;
  std::optional<MediaPresentationRange> presentation_range;
  std::optional<std::pair<std::int64_t, std::int64_t>> coverage;
  std::vector<Mp4Sample> samples;
  std::vector<std::uint32_t> sync;
  std::uint32_t description_count = 1;
  std::uint32_t default_description = 1;
  std::uint32_t default_duration = 0;
  std::uint32_t default_size = 0;
  std::uint32_t default_flags = 0;
  std::optional<std::int64_t> next_dts;
  bool defaults = false;

  MediaTimestamp Time(std::int64_t ticks) const { return {ticks, scale}; }
};

void IndexSyncSamples(TrackIndex& track) {
  for (std::uint32_t i = 0; i < track.samples.size(); ++i) {
    if (track.samples[i].sync) track.sync.push_back(i);
  }
  std::stable_sort(track.sync.begin(), track.sync.end(), [&](auto first, auto second) {
    return track.samples[first].pts < track.samples[second].pts;
  });
}

template<class Visit> void VisitChunks(ByteView stbl, std::uint32_t count, std::uint32_t descriptions, Visit visit) {
  const auto offsets64 = Child(stbl, "co64", false);
  const auto offsets = offsets64.empty() ? Child(stbl, "stco") : offsets64;
  const auto stride = offsets64.empty() ? 4 : 8;
  const auto chunk_count = Entries(offsets, stride);
  const auto mapping = Child(stbl, "stsc");
  const auto mappings = Entries(mapping, 12);
  std::uint32_t index = 0;
  std::size_t entry = 0;
  std::uint32_t per_chunk = 0;
  std::uint32_t description = 0;
  std::uint64_t previous_end = 0;
  for (std::size_t chunk = 1; chunk <= chunk_count; ++chunk) {
    const auto position = 8 + entry * 12;
    if (entry < mappings && Number<std::uint32_t>(mapping, position) == chunk) {
      per_chunk = Number<std::uint32_t>(mapping, position + 4);
      description = Number<std::uint32_t>(mapping, position + 8);
      if (!description || description > descriptions) Fail(Invalid, "Invalid MP4 chunk sample description index");
      ++entry;
    }
    if (!per_chunk || per_chunk > count - index) Fail(Invalid, "Invalid MP4 samples per chunk");
    auto offset = stride == 4 ? Number<std::uint32_t>(offsets, 8 + (chunk - 1) * stride)
                             : Number<std::uint64_t>(offsets, 8 + (chunk - 1) * stride);
    if (offset < previous_end) Fail(Unsupported, "MP4 track chunks are not stored in decode order");
    previous_end = visit(index, per_chunk, offset, description);
    index += per_chunk;
  }
  if (entry != mappings || index != count) Fail(Invalid, "MP4 chunk table does not cover all samples");
}

void ParsePcmSamples(ByteView stbl, TrackIndex& index, const MediaSampleDescription& track, std::size_t remaining) {
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
  VisitChunks(stbl, count, 1, [&](std::uint32_t, std::uint32_t frames, std::uint64_t offset, std::uint32_t) {
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

  VisitChunks(stbl, count, track.description_count,
      [&](std::uint32_t first, std::uint32_t per_chunk, std::uint64_t offset, std::uint32_t description) {
    for (std::uint32_t i = 0; i < per_chunk; ++i) {
      auto& sample = track.samples[first + i];
      sample.offset = offset;
      sample.description = description;
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
  IndexSyncSamples(track);
}

struct SampleRef {
  std::uint32_t track;
  std::uint32_t index;
};

class Mp4Reader final : public ContainerReader {
public:
  Mp4Reader(ContainerInput input, std::optional<ContainerFormat> hint, ContainerInputMode mode)
      : ContainerReader(std::move(input), ContainerFormat::Mp4), growing_(mode == ContainerInputMode::Growing) {
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
        fragment_start_ = this->input.Position();
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
    if (growing_ && (layout != ContainerLayout::Fragmented || !this->input.Size()))
      Fail(Unsupported, "Growing input requires a sized random-access fragmented MP4");
    movie_duration_ = duration;
    if (layout == ContainerLayout::Default) {
      order_ = OrderSamples(indexes_);
      if (this->input.Random()) ValidateMedia(indexes_, order_, media_);
    } else {
      read_offset_ = fragment_start_;
      if (!media_.empty()) read_media_ = index_media_ = media_.back();
    }
    if (growing_) EnableFragmentIndex();
  }

  std::optional<ContainerPacket> Read() override {
    const bool indexed = layout == ContainerLayout::Default || fragment_indexed_;
    const bool seekable = input.Random() && (indexed || input.Size());
    for (;;) {
      if (seekable) {
        while (cursor_ < order_.size() && (Sample(indexes_, order_[cursor_]).offset < read_floor_ ||
              (!selected.empty() && std::ranges::find(selected, tracks[order_[cursor_].track].id) == selected.end())))
          ++cursor_;
      }
      if (cursor_ < order_.size()) break;
      if (fragment_indexed_) {
        if (growing_) input.RefreshSize();
        if (!IndexNextFragment()) return {};
      } else if (indexed) {
        return {};
      } else if (!(seekable ? NextReadFragment() : NextFragment())) {
        if (whole_stream_) PublishFragmentCoverage(true);
        return {};
      }
    }
    const auto ref = order_[cursor_++];
    const auto& sample = Sample(indexes_, ref);
    if (tracks[ref.track].descriptions[sample.description - 1].sample_entry == "tmcd" && sample.size != 4)
      Fail(Invalid, "QuickTime timecode requires four-byte samples");
    if (seekable) {
      input.Seek(sample.offset);
    } else {
      PositionSample(sample);
    }
    auto& track = indexes_[ref.track];
    auto data = Own(input.ReadBytes(sample.size));
    if (!seekable && !indexed && whole_stream_) UpdateCoverage(track, std::span(&sample, 1));
    const auto pts = track.Time(sample.pts), dts = track.Time(sample.dts);
    return ContainerPacket{tracks[ref.track].id, std::move(data), pts, dts,
                           MediaTimestamp{sample.duration, track.scale}, sample.sync, {}, sample.description};
  }

  std::optional<MediaPresentationRange> PresentationRange(std::size_t track) const override {
    return indexes_[track].presentation_range;
  }

  std::optional<SeekPoint> Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) override {
    if (seek && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "MP4 input cannot seek");
    const auto index = PacketTrack(tracks, id);
    if (layout == ContainerLayout::Fragmented) {
      if (auto point = FindEmbedded(index, time, direction, seek)) return point;
      EnableFragmentIndex();
      if (growing_) input.RefreshSize();
      while (!IndexCovers(index, time, direction) && IndexNextFragment()) {}
    }
    const auto& track = indexes_[index];
    const auto anchor = FindAnchor(track, time, direction);
    if (!anchor) return {};
    const auto decode = DecodeStart(index, track, anchor->sample);
    auto point = SeekSample(index, track, *anchor, decode);
    if (seek) {
      const auto entry = std::lower_bound(order_.begin(), order_.end(), track.samples[decode].offset,
          [&](SampleRef ref, std::uint64_t offset) { return Sample(indexes_, ref).offset < offset; });
      cursor_ = static_cast<std::size_t>(entry - order_.begin());
      read_floor_ = 0;
    }
    return point;
  }

private:
  struct Anchor {
    std::uint32_t sample;
    MediaTimestamp presentation_time;
    bool preroll;
  };

  static std::optional<Anchor> FindAnchor(const TrackIndex& track, MediaTimestamp time, SeekDirection direction) {
    if (!track.timeline) {
      if (const auto sample = FindSample(track, time, direction))
        return Anchor{*sample, track.Time(track.samples[*sample].pts), false};
      return {};
    }
    const auto& timeline = *track.timeline;
    const bool forward = direction == SeekDirection::AtOrAfter;
    auto find_in = [&](const Mp4EditSegment& segment) -> std::optional<Anchor> {
      const auto clock = timeline.scale;
      const MediaTimestamp begin{segment.begin, clock}, end{segment.end, clock};
      if (forward ? time.Compare(end) >= 0 : time.Compare(begin) < 0) return {};
      if (!track.coverage || track.Time(track.coverage->second).Compare({segment.media, clock}) <= 0 ||
          track.Time(track.coverage->first).Compare({AddTime(segment.media, segment.end - segment.begin), clock}) >= 0)
        return {};
      const auto ticks = time.Compare(begin) < 0 ? segment.begin : time.Compare(end) >= 0 ? segment.end - 1 :
          DirectionalTicks(time, clock, forward);
      const auto media = AddTime(segment.media, ticks - segment.begin);
      auto sample = forward && time.Compare(begin) <= 0
          ? FindSample(track, {media, clock}, SeekDirection::AtOrBefore) : std::nullopt;
      if (!sample) sample = FindSample(track, {media, clock}, direction);
      if (!sample) return {};
      const auto pts = ExactTicks(track.Time(track.samples[*sample].pts), clock);
      if (pts >= AddTime(segment.media, segment.end - segment.begin)) return {};
      const auto projected = AddTime(segment.begin, SubTime(pts, segment.media));
      return Anchor{*sample, {std::max(projected, segment.begin), clock}, projected < segment.begin};
    };
    if (forward) {
      for (const auto& segment : timeline.segments)
        if (auto anchor = find_in(segment)) return anchor;
    } else {
      for (auto segment = timeline.segments.rbegin(); segment != timeline.segments.rend(); ++segment)
        if (auto anchor = find_in(*segment)) return anchor;
    }
    return {};
  }

  static std::optional<std::uint32_t> FindSample(const TrackIndex& track, MediaTimestamp target,
                                                 SeekDirection direction) {
    const auto& points = track.sync;
    if (direction == SeekDirection::AtOrAfter) {
      const auto at = std::lower_bound(points.begin(), points.end(), target, [&](auto sample, auto value) {
        return track.Time(track.samples[sample].pts).Compare(value) < 0;
      });
      if (at != points.end()) return *at;
    } else {
      const auto at = std::upper_bound(points.begin(), points.end(), target, [&](auto value, auto sample) {
        return value.Compare(track.Time(track.samples[sample].pts)) < 0;
      });
      if (at != points.begin()) return *std::prev(at);
    }
    return {};
  }

  bool IsAac(std::size_t index, const Mp4Sample& sample) const {
    return tracks[index].descriptions[sample.description - 1].codec == CodecId::Aac;
  }

  std::uint32_t DecodeStart(std::size_t index, const TrackIndex& track, std::uint32_t sample) const {
    if (!sample || !IsAac(index, track.samples[sample])) return sample;
    const auto& previous = track.samples[sample - 1];
    const auto& anchor = track.samples[sample];
    // AAC needs the preceding access unit for overlap when it belongs to the same decoding sequence.
    return previous.description == anchor.description && AddTime(previous.dts, previous.duration) == anchor.dts
        ? sample - 1 : sample;
  }

  SeekPoint SeekSample(std::size_t index, const TrackIndex& track, const Anchor& anchor, std::uint32_t decode) {
    const auto& sample = track.samples[anchor.sample];
    const auto& description = tracks[index].descriptions[sample.description - 1];
    auto kind = description.framing == PacketFraming::LengthPrefixedNals ? SeekPoint::Kind::SyncMarked
                                                                        : SeekPoint::Kind::Independent;
    const bool avc = description.codec == CodecId::H264;
    const bool hvc1 = description.codec == CodecId::Hevc && description.sample_entry == "hvc1";
    if ((avc || hvc1) && input.Random()) {
      // Inspect the sync sample without changing the session's observable read position.
      const auto previous = input.Position();
      input.Seek(sample.offset);
      try {
        const auto bytes = input.ReadBytes(sample.size);
        const auto length = (Number<std::uint8_t>(description.initialization_data, avc ? 4 : 21) & 3) + 1;
        const bool idr = avc ? AvcIdr(bytes, length, Invalid) : HevcIdr(bytes, length, true, Invalid);
        if (idr) kind = SeekPoint::Kind::Independent;
      } catch (...) {
        input.Seek(previous);
        throw;
      }
      input.Seek(previous);
    }
    if (decode != anchor.sample) kind = SeekPoint::Kind::NeedsPreroll;
    return {tracks[index].id, anchor.presentation_time, kind, sample.offset, track.Time(track.samples[decode].pts)};
  }

  static std::int64_t DirectionalTicks(MediaTimestamp time, std::uint32_t scale, bool forward) {
    const auto value = time.Rescale(scale);
    if (!value) Fail(ContainerErrorCode::LimitExceeded, "MP4 seek time is outside its clock");
    const auto relation = value->Compare(time);
    return AddTime(value->ticks, forward && relation < 0 ? 1 : !forward && relation > 0 ? -1 : 0);
  }

  static const Mp4Sample& Sample(const std::vector<TrackIndex>& indexes, SampleRef ref) {
    return indexes[ref.track].samples[ref.index];
  }

  static std::vector<SampleRef> OrderSamples(const std::vector<TrackIndex>& indexes) {
    std::vector<SampleRef> order;
    for (std::uint32_t i = 0; i < indexes.size(); ++i) {
      for (std::uint32_t j = 0; j < indexes[i].samples.size(); ++j) order.push_back({i, j});
    }
    std::ranges::sort(order, {}, [&](SampleRef ref) { return Sample(indexes, ref).offset; });
    std::uint64_t end = 0;
    for (const auto ref : order) {
      const auto& sample = Sample(indexes, ref);
      if (sample.offset < end) Fail(Invalid, "MP4 sample byte ranges overlap");
      end = AddOffset(sample.offset, sample.size);
    }
    return order;
  }

  using MediaRange = std::pair<std::uint64_t, std::uint64_t>;
  using MediaRanges = std::vector<MediaRange>;

  static void ValidateMedia(const std::vector<TrackIndex>& indexes, std::span<const SampleRef> order,
                            const MediaRanges& media) {
    std::size_t range = 0;
    for (auto ref : order) {
      const auto& sample = Sample(indexes, ref);
      while (range < media.size() && sample.offset >= media[range].second) ++range;
      if (range == media.size() || sample.offset < media[range].first ||
          AddOffset(sample.offset, sample.size) > media[range].second)
        Fail(Invalid, "MP4 sample lies outside media data");
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
    auto boxes = movie;
    while (!boxes.empty()) {
      const auto box = NextBox(boxes);
      if (box.tag == Tag("trak")) ReadTrack(box.bytes, count);
    }
    if (tracks.empty()) Fail(Invalid, "Movie has no supported tracks");
    if (format == ContainerFormat::Mov) {
      for (std::size_t i = 0; i < tracks.size(); ++i) {
        auto& refs = tracks[i].references;
        std::erase_if(refs, [&](const auto& ref) {
          return std::ranges::find(tracks, ref.track_id, &MediaTrack::id) == tracks.end();
        });
      }
    }
    if (layout == ContainerLayout::Fragmented) {
      if (count) Fail(Unsupported, "MP4 hybrid sample tables and fragments are unsupported");
      ReadDefaults(extensions);
    }
    if (format == ContainerFormat::Mp4 && tracks.size() == 1 && tracks.front().kind == TrackKind::Audio)
      audio_metadata = ReadAudioMetadata(movie);
  }

  void ReadTrack(ByteView box, std::size_t& count) {
    const auto mdia = Child(box, "mdia");
    const auto handler = Number<std::uint32_t>(Child(mdia, "hdlr"), 8);
    const bool data = format == ContainerFormat::Mov &&
        (handler == Tag("tmcd") || handler == Tag("meta") || handler == Tag("mdta"));
    const auto minf = Child(mdia, "minf", !data);
    const auto stbl = Child(minf, "stbl", !data);
    const auto description = Child(stbl, "stsd", !data);
    if (data && (description.size() < 16 ||
        Number<std::uint32_t>(description, 12) != (handler == Tag("tmcd") ? Tag("tmcd") : Tag("mebx")))) return;
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
    index.scale = Number<std::uint32_t>(mdhd, Version(mdhd) ? 20 : 12);
    if (!index.scale) Fail(Invalid, "Zero MP4 track timescale");
    track.timescale = index.scale;
    const auto references = Child(Child(minf, "dinf"), "dref");
    if (Number<std::uint32_t>(references, 4) != 1)
      Fail(Unsupported, "MP4 external or multiple data references are unsupported");
    auto urls = references.subspan(8);
    const auto url = NextBox(urls);
    if ((url.tag != Tag("url ") && !(format == ContainerFormat::Mov && url.tag == Tag("alis"))) ||
        Number<std::uint32_t>(url.bytes) != 1 || !urls.empty())
      Fail(Unsupported, "MP4 external data reference");
    CheckVersion(description, 0);
    index.description_count = Number<std::uint32_t>(description, 4);
    if (!index.description_count) Fail(Invalid, "MP4 track has no sample descriptions");
    if (index.description_count > MaxMp4Descriptions)
      Fail(ContainerErrorCode::LimitExceeded, "MP4 track has too many sample descriptions");
    if (format == ContainerFormat::Mov) track.references = ReadReferences(Child(box, "tref", false));
    auto entries = description.subspan(8);
    track.kind = handler == Tag("vide") ? TrackKind::Video : handler == Tag("soun") ? TrackKind::Audio : TrackKind::Data;
    if (track.kind == TrackKind::Video) {
      const auto rotation = Rotation(tkhd, Version(tkhd) ? 52 : 40);
      track.rotation = static_cast<VideoRotation>(
          (static_cast<unsigned>(movie_rotation_) + static_cast<unsigned>(rotation)) % 4);
    }
    auto& descriptions = track.descriptions;
    descriptions.reserve(index.description_count);
    for (std::uint32_t i = 1; i <= index.description_count; ++i) {
      const auto entry = NextBox(entries);
      if (data && (handler == Tag("tmcd") ? entry.tag != Tag("tmcd") : entry.tag != Tag("mebx"))) return;
      if (Number<std::uint16_t>(entry.bytes, 6) != 1)
        Fail(Unsupported, "Unsupported MP4 sample description reference");
      MediaSampleDescription described;
      ReadDescription(entry, handler, described);
      if (!descriptions.empty()) {
        const auto& first = descriptions.front();
        if ((first.codec != CodecId::H264 && first.codec != CodecId::Hevc && first.codec != CodecId::Aac) ||
            described.codec != first.codec || described.sample_rate != first.sample_rate ||
            described.channel_count != first.channel_count)
          Fail(Unsupported, "MP4 sample descriptions require one codec and audio format per track");
      }
      descriptions.push_back(std::move(described));
    }
    if (!entries.empty()) Fail(Invalid, "Unexpected bytes after MP4 sample descriptions");
    const auto& primary = descriptions.front();
    if (MovPcmBits(primary.codec)) {
      ParsePcmSamples(stbl, index, primary, MaxContainerSamples - count);
    } else {
      ParseSamples(stbl, index, MaxContainerSamples - count);
    }
    track.edits = ParseEdits(Child(box, "edts", false), movie_scale_, index.scale);
    if (!track.edits.empty()) index.timeline = Mp4Edits(track.edits, movie_scale_, index.scale, Invalid);
    UpdateCoverage(index, index.samples);
    SetPresentation(index);
    if (const auto& window = index.presentation_range; window && window->start != window->end &&
        (!start || window->start.Compare(*start) < 0)) start = window->start;
    count += index.samples.size();
    tracks.push_back(std::move(track));
    indexes_.push_back(std::move(index));
  }

  void ReadDescription(const BoxView& entry, std::uint32_t handler, MediaSampleDescription& track) {
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
      track.codec = CodecId::Aac;
      track.sample_rate = config.Value().sample_rate;
      track.channel_count = config.Value().channels;
    } else if (handler == Tag("soun") && entry.tag == Tag("alac") && format == ContainerFormat::Mp4) {
      if (Number<std::uint16_t>(entry.bytes, 8) != 0) Fail(Unsupported, "Unsupported ALAC audio sample entry version");
      Slice(entry.bytes, 0, 28);
      const auto full = Child(entry.bytes.subspan(28), "alac");
      if (Number<std::uint32_t>(full) != 0) Fail(Unsupported, "Unsupported ALAC configuration box version");
      const auto config = full.subspan(4);
      const auto alac = ParseAlac(config, Invalid);
      track.codec = CodecId::Alac;
      track.sample_rate = alac.sample_rate;
      track.channel_count = alac.channels;
      track.initialization_data.assign(config.begin(), config.end());
    } else if (handler == Tag("vide")) {
      const auto width = Number<std::uint16_t>(entry.bytes, 24), height = Number<std::uint16_t>(entry.bytes, 26);
      if (!width || !height) Fail(Invalid, "Invalid MP4 visual sample dimensions");
      track.coded_width = width;
      track.coded_height = height;
      Slice(entry.bytes, 0, 78);
      const auto extensions = entry.bytes.subspan(78);
      const bool mov_visual = format == ContainerFormat::Mov;
      ByteView config;
      if (entry.tag == Tag("avc1")) {
        track.codec = CodecId::H264;
        config = Child(extensions, "avcC", true, mov_visual);
        const auto avc = ParseAvc(config);
        if (!avc.Succeeded()) Fail(Unsupported, avc.Error().message);
      } else if (entry.tag == Tag("hvc1") || entry.tag == Tag("hev1")) {
        track.codec = CodecId::Hevc;
        track.sample_entry = EntryName(entry.tag);
        config = Child(extensions, "hvcC", true, mov_visual);
        HevcNalLength(config, entry.tag == Tag("hvc1"), Invalid);
      } else if (format == ContainerFormat::Mov && ProResEntry(entry.tag)) {
        track.codec = CodecId::ProRes;
        track.sample_entry = EntryName(entry.tag);
      } else {
        Fail(Unsupported, "Unsupported MP4 or QuickTime video sample entry");
      }
      if (!config.empty()) {
        track.framing = PacketFraming::LengthPrefixedNals;
        track.initialization_data.assign(config.begin(), config.end());
      }
      ReadVisualMetadata(extensions, track, mov_visual);
    } else if (format == ContainerFormat::Mov && (entry.tag == Tag("tmcd") || entry.tag == Tag("mebx"))) {
      track.framing = PacketFraming::ContainerSample;
      track.sample_entry = EntryName(entry.tag);
      const auto data = entry.bytes.subspan(8);
      ValidateMovDataDescription(track.sample_entry, data, Invalid);
      track.initialization_data.assign(data.begin(), data.end());
    } else if (handler == Tag("soun") && format == ContainerFormat::Mov) {
      ReadPcmDescription(entry, track);
    } else {
      Fail(Unsupported, "Unsupported MP4 or QuickTime track");
    }
  }

  static std::vector<MediaTrack::Reference> ReadReferences(ByteView boxes) {
    std::vector<MediaTrack::Reference> result;
    while (!boxes.empty()) {
      const auto box = NextBox(boxes);
      if (box.tag != Tag("tmcd") && box.tag != Tag("cdsc")) continue;
      if (box.bytes.size() % 4 || box.bytes.size() / 4 > MaxContainerTracks - result.size())
        Fail(Invalid, "Invalid QuickTime track reference table");
      for (std::size_t offset = 0; offset < box.bytes.size(); offset += 4) {
        const auto id = Number<std::uint32_t>(box.bytes, offset);
        if (id) result.push_back({EntryName(box.tag), id});
      }
    }
    return result;
  }

  static void ReadVisualMetadata(ByteView extensions, MediaSampleDescription& track, bool mov_visual) {
    while (!extensions.empty()) {
      if (mov_visual && extensions.size() == 4 && Number<std::uint32_t>(extensions) == 0) break;
      const auto box = NextBox(extensions);
      if (box.tag == Tag("clap")) {
        if (track.visible) Fail(Invalid, "Duplicate MP4 clean aperture");
        track.visible = Aperture(box.bytes, *track.coded_width, *track.coded_height);
      } else if (box.tag == Tag("pasp")) {
        if (track.pixel_aspect_ratio || box.bytes.size() != 8)
          Fail(Invalid, "Duplicate or malformed MP4 pixel aspect ratio");
        const auto width = Number<std::uint32_t>(box.bytes), height = Number<std::uint32_t>(box.bytes, 4);
        if (!width || !height) Fail(Invalid, "Zero MP4 pixel aspect spacing");
        track.pixel_aspect_ratio = MediaSampleDescription::PixelAspectRatio{width, height};
      } else if (Mp4VideoExtensionTag(box.tag)) {
        auto type = EntryName(box.tag);
        if (std::ranges::find(track.video_extensions, type, &MediaSampleDescription::VideoExtension::type) != track.video_extensions.end())
          Fail(Invalid, "Duplicate MP4 visual extension");
        ValidateMp4VideoExtension(box.tag, box.bytes, track.codec, Invalid);
        track.video_extensions.push_back({std::move(type), Bytes(box.bytes.begin(), box.bytes.end())});
      }
    }
  }

  static void UpdateCoverage(TrackIndex& index, std::span<const Mp4Sample> samples) {
    for (const auto& sample : samples) {
      const auto end = AddTime(sample.pts, sample.duration);
      if (!index.coverage) index.coverage = {sample.pts, end};
      else {
        index.coverage->first = std::min(index.coverage->first, sample.pts);
        index.coverage->second = std::max(index.coverage->second, end);
      }
    }
  }

  static void SetPresentation(TrackIndex& index) {
    index.presentation_range.reset();
    if (!index.coverage) return;
    const auto [first, last] = *index.coverage;
    if (const auto& timeline = index.timeline) {
      const auto first_tick = ExactTicks({first, index.scale}, timeline->scale);
      const auto end_tick = ExactTicks({last, index.scale}, timeline->scale);
      for (const auto& segment : timeline->segments) {
        const auto low = std::max(first_tick, segment.media);
        const auto high = std::min(end_tick, AddTime(segment.media, segment.end - segment.begin));
        if (high <= low) continue;
        const MediaTimestamp begin{AddTime(segment.begin, low - segment.media), timeline->scale};
        const MediaTimestamp end{AddTime(segment.begin, high - segment.media), timeline->scale};
        if (!index.presentation_range) index.presentation_range = MediaPresentationRange{begin, end};
        else index.presentation_range->end = end;
      }
      if (!index.presentation_range) {
        const MediaTimestamp begin{timeline->segments.front().begin, timeline->scale};
        index.presentation_range = MediaPresentationRange{begin, begin};
      }
    } else {
      index.presentation_range = MediaPresentationRange{index.Time(first), index.Time(last)};
    }
  }

  void ReadDefaults(ByteView boxes) {
    while (!boxes.empty()) {
      const auto box = NextBox(boxes);
      if (box.tag != Tag("trex")) continue;
      CheckVersion(box.bytes, 0);
      const auto id = Number<std::uint32_t>(box.bytes, 4);
      const auto i = FragmentTrack(id);
      auto& track = indexes_[i];
      track.default_description = Number<std::uint32_t>(box.bytes, 8);
      if (track.defaults || !track.default_description || track.default_description > track.description_count)
        Fail(Invalid, "Invalid MP4 fragment default description");
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

  void EnableFragmentIndex() {
    if (fragment_indexed_) return;
    if (!input.Random() || !input.Size())
      Fail(ContainerErrorCode::IndexUnavailable, "Fragmented MP4 indexing requires a sized random-access input");
    if (cursor_ < order_.size()) {
      read_floor_ = Sample(indexes_, order_[cursor_]).offset;
    } else if (!order_.empty()) {
      const auto& last = Sample(indexes_, order_.back());
      read_floor_ = AddOffset(last.offset, last.size);
    } else {
      read_floor_ = input.Position();
    }
    for (auto& index : indexes_) {
      index.samples.clear();
      index.sync.clear();
      index.next_dts.reset();
    }
    order_.clear();
    cursor_ = 0;
    scan_offset_ = fragment_start_;
    fragment_indexed_ = true;
  }

  bool IndexCovers(std::size_t id, MediaTimestamp time, SeekDirection direction) const {
    if (scan_offset_ == *input.Size()) return true;
    const auto& track = indexes_[id];
    const auto anchor = FindAnchor(track, time, direction);
    if (anchor && !anchor->preroll && anchor->presentation_time == time) return true;
    if (!track.next_dts) return false;
    // An unseen sync picture can have the minimum signed composition offset.
    const auto lower_bound = track.Time(*track.next_dts + std::int64_t(INT32_MIN));
    if (direction == SeekDirection::AtOrAfter)
      return anchor && !anchor->preroll && lower_bound.Compare(track.Time(track.samples[anchor->sample].pts)) >= 0;
    if (!track.timeline) return lower_bound.Compare(time) > 0;
    const auto& [scale, segments, duration] = *track.timeline;
    for (auto segment = segments.rbegin(); segment != segments.rend(); ++segment) {
      if (time.Compare({segment->begin, scale}) < 0) continue;
      const auto position = time.Compare({segment->end, scale}) >= 0
          ? segment->end - 1 : DirectionalTicks(time, scale, false);
      return lower_bound.Compare({AddTime(segment->media, position - segment->begin), scale}) > 0;
    }
    return true;
  }

  struct FragmentBatch {
    std::vector<TrackIndex> tracks;
    std::vector<SampleRef> order;
    std::optional<MediaRange> remaining_media;
    std::uint64_t media_start = 0;
  };

  std::optional<FragmentBatch> ReadFragmentBatch(std::uint64_t limit, std::size_t& boxes,
                                                 std::optional<MediaRange> preceding, bool standalone = false) {
    MediaRanges media;
    if (preceding) media.push_back(*preceding);
    std::optional<FragmentBatch> batch;
    std::size_t count = 0;
    std::uint64_t required_end = 0;
    while (input.Position() < limit) {
      const auto box = ReadBox(input, growing_);
      if (!box) break;
      if (++boxes > 4096) Fail(ContainerErrorCode::LimitExceeded, "Too many boxes between MP4 fragments");
      const auto end = AddOffset(input.Position(), box->size);
      if (end > limit) Fail(Invalid, "MP4 fragment exceeds its index range");
      if (box->tag == Tag("moof")) {
        if (batch && standalone) return {};
        if (!batch) {
          batch.emplace();
          batch->tracks.reserve(indexes_.size());
          for (const auto& index : indexes_) {
            batch->tracks.push_back({.scale = index.scale,
                .description_count = index.description_count, .default_description = index.default_description,
                .default_duration = index.default_duration, .default_size = index.default_size, .default_flags = index.default_flags,
                .next_dts = standalone ? std::nullopt : index.next_dts});
          }
          batch->media_start = end;
        }
        Fragment(ReadMetadata(input, box->size), box->offset, batch->tracks, count, standalone);
        for (const auto& track : batch->tracks) {
          if (track.samples.empty()) continue;
          const auto& last = track.samples.back();
          required_end = std::max(required_end, AddOffset(last.offset, last.size));
        }
      } else if (box->tag == Tag("mdat")) {
        media.emplace_back(input.Position(), end);
      }
      input.Seek(end);
      if (batch && (!count || (!media.empty() && required_end <= media.back().second))) {
        batch->order = OrderSamples(batch->tracks);
        ValidateMedia(batch->tracks, batch->order, media);
        if (!media.empty()) {
          auto [begin, end] = media.back();
          if (!batch->order.empty()) {
            const auto& last = Sample(batch->tracks, batch->order.back());
            begin = std::max(begin, AddOffset(last.offset, last.size));
          }
          if (begin < end) batch->remaining_media.emplace(begin, end);
        }
        return batch;
      }
    }
    if (batch && !growing_) Fail(Invalid, "MP4 fragment has no matching media data");
    return {};
  }

  bool NextReadFragment() {
    input.Seek(read_offset_);
    std::size_t boxes = 0;
    auto batch = ReadFragmentBatch(*input.Size(), boxes, read_media_);
    if (!batch) return false;
    read_offset_ = input.Position();
    read_media_ = batch->remaining_media;
    for (std::size_t i = 0; i < indexes_.size(); ++i) {
      auto& track = indexes_[i];
      auto& fragment = batch->tracks[i];
      if (whole_stream_) UpdateCoverage(track, fragment.samples);
      track.samples = std::move(fragment.samples);
      track.next_dts = fragment.next_dts;
    }
    order_ = std::move(batch->order);
    cursor_ = 0;
    if (whole_stream_) PublishFragmentCoverage(false);
    return true;
  }

  struct SegmentReference {
    std::uint64_t offset, end_offset;
    MediaTimestamp start, end;
  };

  std::vector<SegmentReference> ReadSegmentIndex(ByteView bytes, std::uint64_t end) {
    CheckVersion(bytes, 1);
    if (Number<std::uint32_t>(bytes, 4) != tracks.front().id) return {};
    const auto scale = Number<std::uint32_t>(bytes, 8);
    if (!scale) return {};
    const bool wide = Version(bytes) != 0;
    auto time = wide ? SignedTicks(Number<std::uint64_t>(bytes, 12)) : Number<std::uint32_t>(bytes, 12);
    auto offset = AddOffset(end, wide ? Number<std::uint64_t>(bytes, 20) : Number<std::uint32_t>(bytes, 16));
    const auto count = Number<std::uint16_t>(bytes, wide ? 30 : 22);
    auto entries = bytes.subspan(wide ? 32 : 24);
    if (entries.size() != std::size_t(count) * 12) return {};
    std::vector<SegmentReference> refs;
    for (std::size_t i = 0; i < count; ++i) {
      const auto size = Number<std::uint32_t>(entries, i * 12);
      const auto length = Number<std::uint32_t>(entries, i * 12 + 4);
      const auto sap = Number<std::uint32_t>(entries, i * 12 + 8);
      if (!size || (size & 0x80000000u) || !length || (sap & 0x8fffffffu) != 0x80000000u)
        return {};
      const auto next = AddOffset(offset, size);
      if (next > *input.Size()) return {};
      const auto finish = AddTime(time, length);
      refs.push_back({offset, next, {time, scale}, {finish, scale}});
      offset = next;
      time = finish;
    }
    return refs;
  }

  std::optional<SeekPoint> FindEmbedded(std::size_t index, MediaTimestamp target, SeekDirection direction, bool seek) {
    if (growing_ || fragment_indexed_ || !input.Size() || indexes_.size() != 1) return {};
    const auto& timeline = indexes_[index].timeline;
    if (timeline) {
      const auto& edit = timeline->segments.front();
      if (timeline->segments.size() != 1 || edit.media != edit.begin || target.Compare({edit.begin, timeline->scale}) < 0 ||
          target.Compare({edit.end, timeline->scale}) >= 0) return {};
    }
    const auto previous = input.Position();
    std::optional<FragmentBatch> batch;
    std::optional<SeekPoint> point;
    std::uint64_t next_offset = 0;
    std::uint64_t decode_offset = 0;
    try {
      if (!segment_index_) {
        segment_index_.emplace();
        input.Seek(fragment_start_);
        for (unsigned boxes = 0; boxes < 64; ++boxes) {
          const auto box = ReadBox(input);
          if (!box || box->tag == Tag("moof") || box->tag == Tag("mdat")) break;
          const auto end = AddOffset(input.Position(), box->size);
          if (box->tag == Tag("sidx")) {
            *segment_index_ = ReadSegmentIndex(ReadMetadata(input, box->size), end);
            break;
          }
          input.Seek(end);
        }
      }
      const auto ref = std::upper_bound(segment_index_->begin(), segment_index_->end(), target,
          [](auto time, const auto& entry) { return time.Compare(entry.end) < 0; });
      if (ref != segment_index_->end() && target.Compare(ref->start) >= 0) {
        input.Seek(ref->offset);
        std::size_t boxes = 0;
        batch = ReadFragmentBatch(ref->end_offset, boxes, {}, true);
        if (batch && input.Position() == ref->end_offset && !batch->order.empty() &&
            Sample(batch->tracks, batch->order.front()).offset >= batch->media_start) {
          next_offset = ref->end_offset;
          auto& track = batch->tracks[index];
          IndexSyncSamples(track);
          UpdateCoverage(track, track.samples);
          if (!track.sync.empty() && track.Time(track.samples[track.sync.front()].pts) == ref->start &&
              track.Time(track.coverage->first) == ref->start && track.Time(track.coverage->second) == ref->end) {
            if (auto candidate = FindSample(track, target, direction)) {
              const auto& sample = track.samples[*candidate];
              const auto time = track.Time(sample.pts);
              const bool inside_edit = !timeline || (time.Compare({timeline->segments.front().begin, timeline->scale}) >= 0 &&
                                                    time.Compare({timeline->segments.front().end, timeline->scale}) < 0);
              // At the start of a later AAC fragment the incremental index can resolve its predecessor.
              if (inside_edit && (*candidate || !IsAac(index, sample) || ref == segment_index_->begin())) {
                const auto decode = DecodeStart(index, track, *candidate);
                point = SeekSample(index, track, {*candidate, time, false}, decode);
                if (point->kind == SeekPoint::Kind::SyncMarked) point.reset();
                else decode_offset = track.samples[decode].offset;
              }
            }
          }
        }
      }
    } catch (const ContainerError& error) {
      input.Seek(previous);
      segment_index_.emplace();
      if (error.code == ContainerErrorCode::Io) throw;
      return {};
    }
    input.Seek(previous);
    if (seek && point) {
      read_offset_ = next_offset;
      read_media_ = batch->remaining_media;
      whole_stream_ = false;
      indexes_[index].samples = std::move(batch->tracks[index].samples);
      indexes_[index].next_dts = batch->tracks[index].next_dts;
      order_ = std::move(batch->order);
      const auto entry = std::lower_bound(order_.begin(), order_.end(), decode_offset,
          [&](SampleRef ref, std::uint64_t offset) { return Sample(indexes_, ref).offset < offset; });
      cursor_ = static_cast<std::size_t>(entry - order_.begin());
    }
    return point;
  }

  void PublishFragmentCoverage(bool complete) {
    std::optional<MediaTimestamp> first, end;
    for (auto& index : indexes_) {
      SetPresentation(index);
      const auto& window = index.presentation_range;
      if (window && window->start != window->end && (!first || window->start.Compare(*first) < 0)) first = window->start;
      const auto track_end = index.timeline ? std::optional(MediaTimestamp{index.timeline->duration, index.timeline->scale})
                                           : window ? std::optional(window->end) : std::nullopt;
      if (track_end && (!end || track_end->Compare(*end) > 0)) end = track_end;
    }
    start = first;
    if (movie_duration_) duration = movie_duration_;
    else if (complete) duration = end;
  }

  bool IndexNextFragment() {
    const auto size = *input.Size();
    if (scan_offset_ == size) return false;
    const auto previous = input.Position();
    std::optional<FragmentBatch> batch;
    std::uint64_t next = 0;
    std::size_t boxes = 0;
    try {
      input.Seek(scan_offset_);
      batch = ReadFragmentBatch(size, boxes, index_media_);
      next = input.Position();
    } catch (...) {
      input.Seek(previous);
      throw;
    }
    input.Seek(previous);
    if (!batch) {
      if (!growing_) {
        scan_offset_ = size;
        PublishFragmentCoverage(true);
      }
      return false;
    }
    if (batch->order.size() > MaxContainerSamples - order_.size() || boxes > 3 * MaxContainerSamples - scan_boxes_)
      Fail(ContainerErrorCode::LimitExceeded, "MP4 fragment index exceeds its limit");
    for (auto& ref : batch->order) ref.index += static_cast<std::uint32_t>(indexes_[ref.track].samples.size());
    for (std::size_t i = 0; i < indexes_.size(); ++i) {
      auto& index = indexes_[i];
      auto& fragment = batch->tracks[i];
      const auto first_sample = static_cast<std::uint32_t>(index.samples.size());
      const auto first_sync = index.sync.size();
      IndexSyncSamples(fragment);
      UpdateCoverage(index, fragment.samples);
      index.samples.insert(index.samples.end(), fragment.samples.begin(), fragment.samples.end());
      for (auto sync : fragment.sync) index.sync.push_back(first_sample + sync);
      const auto ordered = [&](auto a, auto b) { return index.samples[a].pts < index.samples[b].pts; };
      if (first_sync && first_sync < index.sync.size() && ordered(index.sync[first_sync], index.sync[first_sync - 1]))
        std::inplace_merge(index.sync.begin(), index.sync.begin() + first_sync, index.sync.end(), ordered);
      index.next_dts = fragment.next_dts;
    }
    order_.insert(order_.end(), batch->order.begin(), batch->order.end());
    scan_offset_ = next;
    index_media_ = batch->remaining_media;
    scan_boxes_ += boxes;
    PublishFragmentCoverage(!growing_ && scan_offset_ == size);
    return true;
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
        std::size_t samples = 0;
        Fragment(ReadMetadata(input, box->size), box->offset, indexes_, samples);
        order_ = OrderSamples(indexes_);
        cursor_ = 0;
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

  void Fragment(ByteView moof, std::uint64_t offset, std::vector<TrackIndex>& indexes, std::size_t& count,
                bool standalone = false) {
    auto previous_end = offset;
    while (!moof.empty()) {
      const auto box = NextBox(moof);
      if (box.tag != Tag("traf")) continue;
      const auto tfhd = Child(box.bytes, "tfhd");
      CheckVersion(tfhd, 0);
      const auto flags = Number<std::uint32_t>(tfhd);
      if (flags & ~0x03003bu) Fail(Unsupported, "Unsupported MP4 track fragment flags");
      auto& track = indexes[FragmentTrack(Number<std::uint32_t>(tfhd, 4))];
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
      const auto description = field(2, track.default_description);
      if (!description || description > track.description_count) Fail(Invalid, "Invalid MP4 fragment sample description index");
      const auto length = field(8, track.default_duration);
      const auto size = field(16, track.default_size);
      const auto sample_flags = field(32, track.default_flags);
      const auto tfdt = Child(box.bytes, "tfdt", false);
      if (standalone && tfdt.empty()) Fail(Unsupported, "Embedded MP4 index requires fragment decode time");
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
        ParseRun(child.bytes, track, base, run_offset, dts, length, size, sample_flags, description, count);
      }
      track.next_dts = dts;
      previous_end = run_offset;
    }
  }

  void ParseRun(ByteView run, TrackIndex& track, std::uint64_t base, std::uint64_t& offset,
                std::int64_t& dts, std::uint32_t duration, std::uint32_t size, std::uint32_t sample_flags,
                std::uint32_t description, std::size_t& total) {
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
      track.samples.push_back({offset, bytes, AddTime(dts, composition), dts, ticks, !(attrs & 0x10000), description});
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
  MediaRanges media_;
  std::uint32_t movie_scale_ = 0;
  VideoRotation movie_rotation_ = VideoRotation::None;
  std::optional<MediaTimestamp> movie_duration_;
  std::optional<std::vector<SegmentReference>> segment_index_;
  std::size_t cursor_ = 0;
  std::size_t scan_boxes_ = 0;
  std::uint64_t fragment_start_ = 0;
  std::uint64_t scan_offset_ = 0;
  std::uint64_t read_offset_ = 0;
  std::uint64_t read_floor_ = 0;
  std::optional<MediaRange> read_media_, index_media_;
  std::uint64_t media_end_ = 0;
  bool media_to_eof_ = false;
  bool whole_stream_ = true;
  bool fragment_indexed_ = false;
  bool growing_;
};
} // namespace

std::unique_ptr<ContainerReader> OpenMp4Reader(ContainerInput input, std::optional<ContainerFormat> hint,
                                               ContainerInputMode mode) {
  return std::make_unique<Mp4Reader>(std::move(input), hint, mode);
}
} // namespace huxerui::media::detail
