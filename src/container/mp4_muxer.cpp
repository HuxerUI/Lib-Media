#include "mp4.h"

#include "codec/common.h"

#include <array>

namespace huxerui::media::detail {
namespace {

constexpr auto WriteError = ContainerErrorCode::InvalidArgument;

void Zero(Bytes& bytes, std::size_t size) { bytes.resize(bytes.size() + size); }

void Matrix(Bytes& bytes) {
  for (auto value : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u}) Put(bytes, value);
}

std::uint32_t MovieScale(std::span<const MediaTrack> tracks) {
  std::uint32_t scale = 1;
  for (auto& track : tracks) scale = CommonScale(scale, Mp4Scale(track));
  return scale;
}

Bytes Descriptor(std::uint8_t type, ByteView data) {
  Bytes bytes;
  Put(bytes, type);
  auto size = static_cast<std::uint32_t>(data.size());
  bool started = false;
  for (int shift = 21; shift >= 0; shift -= 7) {
    const auto part = (size >> shift) & 127;
    if (part || started || !shift) {
      Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(part | (shift ? 128 : 0)));
      started = true;
    }
  }
  Append(bytes, data);
  return bytes;
}

void SampleDescription(Bytes& bytes, const MediaTrack& track) {
  FullBox(bytes, "stsd", 0, [&] {
    Put(bytes, 1u);
    if (track.codec == CodecId::Aac) {
      Box(bytes, "mp4a", [&] {
        Zero(bytes, 6);
        Put<std::uint16_t>(bytes, 1);
        Zero(bytes, 8);
        Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(*track.channel_count));
        Put<std::uint16_t>(bytes, 16);
        Zero(bytes, 4);
        Put(bytes, *track.sample_rate << 16);
        FullBox(bytes, "esds", 0, [&] {
          Bytes decoder{std::byte{0x40}, std::byte{0x15}};
          Zero(decoder, 11);
          Append(decoder, Descriptor(5, track.initialization_data));
          Bytes es;
          Put<std::uint16_t>(es, 0);
          Put<std::uint8_t>(es, 0);
          Append(es, Descriptor(4, decoder));
          constexpr std::array sl{std::byte{2}};
          Append(es, Descriptor(6, sl));
          Append(bytes, Descriptor(3, es));
        });
      });
    } else {
      Box(bytes, "avc1", [&] {
        Zero(bytes, 6);
        Put<std::uint16_t>(bytes, 1);
        Zero(bytes, 16);
        Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(*track.coded_width));
        Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(*track.coded_height));
        Put(bytes, 0x00480000u);
        Put(bytes, 0x00480000u);
        Put(bytes, 0u);
        Put<std::uint16_t>(bytes, 1);
        Zero(bytes, 32);
        Put<std::uint16_t>(bytes, 24);
        Put<std::uint16_t>(bytes, 0xffff);
        Box(bytes, "avcC", [&] { Append(bytes, track.initialization_data); });
        if (track.visible && *track.visible != VideoRectangle{0, 0, *track.coded_width, *track.coded_height}) {
          const auto rect = *track.visible;
          Box(bytes, "clap", [&] {
            Put(bytes, rect.width);
            Put(bytes, 1u);
            Put(bytes, rect.height);
            Put(bytes, 1u);
            Put(bytes, static_cast<std::int32_t>(2 * rect.x + rect.width) - static_cast<std::int32_t>(*track.coded_width));
            Put(bytes, 2u);
            Put(bytes, static_cast<std::int32_t>(2 * rect.y + rect.height) - static_cast<std::int32_t>(*track.coded_height));
            Put(bytes, 2u);
          });
        }
      });
    }
  });
}

void SampleTable(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples) {
  Box(bytes, "stbl", [&] {
    SampleDescription(bytes, track);
    const auto count = static_cast<std::uint32_t>(samples.size());
    FullBox(bytes, "stts", 0, [&] {
      const auto entries = bytes.size();
      Put(bytes, 0u);
      std::uint32_t runs = 0;
      for (std::size_t i = 0; i < count;) {
        std::size_t end = i + 1;
        while (end < count && samples[end].duration == samples[i].duration) ++end;
        Put(bytes, static_cast<std::uint32_t>(end - i));
        Put(bytes, samples[i].duration);
        ++runs;
        i = end;
      }
      SetU32(bytes, entries, runs);
    });
    if (std::ranges::any_of(samples, [](const auto& sample) { return sample.pts != sample.dts; })) {
      const bool negative = std::ranges::any_of(samples, [](const auto& sample) { return sample.pts < sample.dts; });
      FullBox(bytes, "ctts", negative ? 0x01000000 : 0, [&] {
        Put(bytes, count);
        for (const auto& sample : samples) {
          Put(bytes, 1u);
          Put(bytes, static_cast<std::int32_t>(sample.pts - sample.dts));
        }
      });
    }
    FullBox(bytes, "stsc", 0, [&] {
      Put(bytes, count ? 1u : 0u);
      if (count) for (int field = 0; field < 3; ++field) Put(bytes, 1u);
    });
    FullBox(bytes, "stsz", 0, [&] {
      Put(bytes, 0u);
      Put(bytes, count);
      for (const auto& sample : samples) Put(bytes, sample.size);
    });
    FullBox(bytes, "co64", 0, [&] {
      Put(bytes, count);
      for (const auto& sample : samples) Put(bytes, sample.offset);
    });
    const auto sync_count = static_cast<std::uint32_t>(std::ranges::count(samples, true, &Mp4Sample::sync));
    if (count && (track.kind == TrackKind::Video || sync_count != count)) {
      FullBox(bytes, "stss", 0, [&] {
        Put(bytes, sync_count);
        for (std::uint32_t i = 0; i < count; ++i) if (samples[i].sync) Put(bytes, i + 1);
      });
    }
  });
}

struct TrackEdit {
  std::int64_t start = 0;
  std::int64_t duration = 0;
  std::int64_t media_start = 0;
  std::int64_t media_duration = 0;
};

TrackEdit Edit(const MediaTrack& track, std::span<const Mp4Sample> samples, std::uint32_t movie_scale) {
  if (samples.empty()) return {};
  const auto scale = Mp4Scale(track);
  const auto first = std::ranges::min_element(samples, {}, &Mp4Sample::pts)->pts;
  auto end = first;
  for (const auto& sample : samples) end = std::max(end, AddTime(sample.pts, sample.duration));
  auto trim = [&](std::uint64_t value) {
    if (!value) return std::int64_t{0};
    if (value > std::numeric_limits<std::int64_t>::max() || !track.sample_rate)
      Fail(WriteError, "Invalid MP4 audio trim");
    return ExactTicks({static_cast<std::int64_t>(value), *track.sample_rate}, scale);
  };
  const auto start = std::max<std::int64_t>(0, AddTime(first, trim(track.priming_samples.value_or(0))));
  end = SubTime(end, trim(track.padding_samples.value_or(0)));
  const auto media_start = SubTime(start, samples.front().dts);
  if (end <= start || media_start < 0)
    Fail(ContainerErrorCode::Unsupported, "MP4 edit cannot represent the requested presentation interval");
  return {ExactTicks({start, scale}, movie_scale),
          ExactTicks({SubTime(end, start), scale}, movie_scale), media_start,
          SubTime(AddTime(samples.back().dts, samples.back().duration), samples.front().dts)};
}

void TrackBox(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples, const TrackEdit& edit) {
  const bool video = track.kind == TrackKind::Video;
  Box(bytes, "trak", [&] {
    FullBox(bytes, "tkhd", 0x01000007, [&] {
      Zero(bytes, 16);
      Put(bytes, track.id);
      Put(bytes, 0u);
      Put(bytes, AddTime(edit.start, edit.duration));
      Zero(bytes, 12);
      Put<std::uint16_t>(bytes, video ? 0 : 0x100);
      Put<std::uint16_t>(bytes, 0);
      Matrix(bytes);
      Put(bytes, track.coded_width.value_or(0) << 16);
      Put(bytes, track.coded_height.value_or(0) << 16);
    });
    if (!samples.empty()) {
      // An optional empty edit preserves the track's offset on the common presentation timeline.
      Box(bytes, "edts", [&] {
        FullBox(bytes, "elst", 0x01000000, [&] {
          Put(bytes, edit.start ? 2u : 1u);
          if (edit.start) {
            Put(bytes, edit.start);
            Put<std::int64_t>(bytes, -1);
            Put(bytes, 0x10000u);
          }
          Put(bytes, edit.duration);
          Put(bytes, edit.media_start);
          Put(bytes, 0x10000u);
        });
      });
    }
    Box(bytes, "mdia", [&] {
      FullBox(bytes, "mdhd", 0x01000000, [&] {
        Zero(bytes, 16);
        Put(bytes, Mp4Scale(track));
        Put(bytes, edit.media_duration);
        Put(bytes, 0x55c40000u);
      });
      FullBox(bytes, "hdlr", 0, [&] {
        Put(bytes, 0u);
        Put(bytes, Tag(video ? "vide" : "soun"));
        Zero(bytes, 13);
      });
      Box(bytes, "minf", [&] {
        FullBox(bytes, video ? "vmhd" : "smhd", video ? 1 : 0, [&] { Zero(bytes, video ? 8 : 4); });
        Box(bytes, "dinf", [&] {
          FullBox(bytes, "dref", 0, [&] {
            Put(bytes, 1u);
            FullBox(bytes, "url ", 1, [] {});
          });
        });
        SampleTable(bytes, track, samples);
      });
    });
  });
}

class Mp4Writer final : public ContainerWriter {
public:
  Mp4Writer(ContainerOutput output, std::span<const MediaTrack> tracks, ContainerLayout layout)
      : ContainerWriter(std::move(output), tracks), fragmented_(layout == ContainerLayout::Fragmented),
        movie_scale_(MovieScale(tracks)), states_(tracks.size()) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].codec == CodecId::H264)
        states_[i].nal_length = ParseAvc(tracks[i].initialization_data).Value().length_size;
    }
    Bytes header;
    Box(header, "ftyp", [&] {
      Put(header, Tag(fragmented_ ? "iso6" : "isom"));
      Put(header, 512u);
      for (auto brand : {"isom", "iso6", "mp41", "avc1"}) Put(header, Tag(brand));
    });
    this->output.Write(header);
    if (fragmented_) {
      this->output.Write(Movie());
    } else {
      // Reserve a 64-bit size field at creation so a large mdat needs no relocation.
      mdat_ = this->output.Position();
      Bytes media;
      Put(media, 1u);
      Put(media, Tag("mdat"));
      Put<std::uint64_t>(media, 0);
      this->output.Write(media);
    }
  }

  bool SupportsTrim() const override { return !fragmented_; }

  std::optional<std::uint32_t> Write(std::uint32_t index, const ContainerPacket& packet) override {
    auto& state = states_[index];
    if (state.pending) {
      for (std::size_t i = 0; i < states_.size(); ++i)
        if (!ended[i] && !states_[i].pending) return tracks[i].id;
      Fail(ContainerErrorCode::InvalidState, "MP4 interleaver has no pending input slot");
    }
    if ((!fragmented_ && samples_ == MaxContainerSamples) || (fragmented_ && sequence_ >= 0xffffffffu - states_.size()))
      Fail(ContainerErrorCode::LimitExceeded, "MP4 sample or fragment limit exceeded");
    const auto& track = tracks[index];
    const auto scale = Mp4Scale(track);
    const bool audio = track.codec == CodecId::Aac;
    const auto dts = packet.dts ? packet.dts : (audio ? packet.pts : std::nullopt);
    if (!packet.pts || !dts || (!packet.duration && !audio))
      Fail(ContainerErrorCode::MissingTiming, "MP4 requires PTS, decode timing, and a positive sample duration");
    const auto pts_ticks = ExactTicks(*packet.pts, scale), dts_ticks = ExactTicks(*dts, scale);
    const auto length = ExactTicks(packet.duration.value_or(MediaTimestamp{1024, track.sample_rate.value_or(1)}), scale);
    const auto composition = SubTime(pts_ticks, dts_ticks);
    if (length <= 0 || length > 0xffffffffu || composition < std::numeric_limits<std::int32_t>::min() ||
        composition > std::numeric_limits<std::int32_t>::max())
      Fail(WriteError, "MP4 sample duration or composition offset is out of range");
    if ((state.next_dts && *state.next_dts != dts_ticks) || (fragmented_ && dts_ticks < 0))
      Fail(ContainerErrorCode::Unsupported, "MP4 requires a continuous decode grid and nonnegative fragment DTS");
    const auto next = AddTime(dts_ticks, length);
    AddTime(pts_ticks, length);
    const auto data = packet.data.AsBytes();
    const bool idr = audio || AvcIdr(data, state.nal_length, WriteError);
    const bool sync = packet.sync_sample.value_or(idr);
    state.bytes.assign(data.begin(), data.end());
    state.pending = Mp4Sample{0, static_cast<std::uint32_t>(data.size()), pts_ticks, dts_ticks,
                             static_cast<std::uint32_t>(length), sync};
    state.next_dts = next;
    ++samples_;
    Drain();
    return {};
  }

  void TrackEnded(std::uint32_t) override { Drain(); }

  void Finish() override {
    std::fill(ended.begin(), ended.end(), true);
    Drain();
    if (!fragmented_) {
      const auto movie = Movie();
      Bytes size;
      Put(size, output.Position() - mdat_);
      output.Patch(mdat_ + 8, size);
      output.Write(movie);
    }
  }

private:
  struct TrackState {
    unsigned nal_length = 0;
    std::optional<std::int64_t> next_dts;
    std::optional<Mp4Sample> pending;
    Bytes bytes;
    std::vector<Mp4Sample> index;
  };

  Bytes Movie() const {
    std::vector<TrackEdit> edits;
    std::int64_t duration = 0;
    std::uint32_t max_id = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto edit = Edit(tracks[i], states_[i].index, movie_scale_);
      edits.push_back(edit);
      duration = std::max(duration, AddTime(edit.start, edit.duration));
      max_id = std::max(max_id, tracks[i].id);
    }
    Bytes bytes;
    Box(bytes, "moov", [&] {
      FullBox(bytes, "mvhd", 0x01000000, [&] {
        Zero(bytes, 16);
        Put(bytes, movie_scale_);
        Put(bytes, duration);
        Put(bytes, 0x10000u);
        Put<std::uint16_t>(bytes, 0x100);
        Zero(bytes, 10);
        Matrix(bytes);
        Zero(bytes, 24);
        Put(bytes, max_id + 1);
      });
      for (std::size_t i = 0; i < tracks.size(); ++i) TrackBox(bytes, tracks[i], states_[i].index, edits[i]);
      if (fragmented_) {
        Box(bytes, "mvex", [&] {
          for (const auto& track : tracks) FullBox(bytes, "trex", 0, [&] {
            Put(bytes, track.id);
            Put(bytes, 1u);
            Zero(bytes, 12);
          });
        });
      }
    });
    return bytes;
  }

  void Drain() {
    for (;;) {
      std::optional<std::size_t> earliest;
      for (std::size_t i = 0; i < states_.size(); ++i) {
        const auto& state = states_[i];
        if (!state.pending) {
          if (!ended[i]) return;
          continue;
        }
        if (!earliest || MediaTimestamp{state.pending->dts, Mp4Scale(tracks[i])}.Compare(
                {states_[*earliest].pending->dts, Mp4Scale(tracks[*earliest])}) < 0) earliest = i;
      }
      if (!earliest) return;
      auto& state = states_[*earliest];
      auto sample = *state.pending;
      // A single slot per live track bounds memory and leaves room for the track requested by TryWrite().
      if (fragmented_) {
        Fragment(tracks[*earliest].id, sample, state.bytes);
      } else {
        sample.offset = output.Position();
        state.index.push_back(sample);
        output.Write(state.bytes);
      }
      state.pending.reset();
      state.bytes.clear();
    }
  }

  void Fragment(std::uint32_t id, const Mp4Sample& sample, ByteView data) {
    Bytes bytes;
    std::size_t offset = 0;
    Box(bytes, "moof", [&] {
      FullBox(bytes, "mfhd", 0, [&] { Put(bytes, ++sequence_); });
      Box(bytes, "traf", [&] {
        FullBox(bytes, "tfhd", 0x20000, [&] { Put(bytes, id); });
        FullBox(bytes, "tfdt", 0x01000000, [&] { Put(bytes, sample.dts); });
        FullBox(bytes, "trun", 0x01000f01, [&] {
          Put(bytes, 1u);
          offset = bytes.size();
          Put(bytes, 0u);
          Put(bytes, sample.duration);
          Put(bytes, sample.size);
          Put(bytes, sample.sync ? 0x02000000u : 0x01010000u);
          Put(bytes, static_cast<std::int32_t>(sample.pts - sample.dts));
        });
      });
    });
    SetU32(bytes, offset, static_cast<std::uint32_t>(bytes.size() + 8));
    Put(bytes, sample.size + 8);
    Put(bytes, Tag("mdat"));
    output.Write(bytes);
    output.Write(data);
  }

  bool fragmented_;
  std::uint32_t movie_scale_;
  std::vector<TrackState> states_;
  std::size_t samples_ = 0;
  std::uint32_t sequence_ = 0;
  std::uint64_t mdat_ = 0;
};
} // namespace

std::uint32_t Mp4Scale(const MediaTrack& track) {
  return track.timescale.value_or(track.sample_rate.value_or(90000));
}

bool AvcIdr(ByteView bytes, unsigned length_size, ContainerErrorCode error) {
  bool idr = false, picture = false;
  while (!bytes.empty()) {
    if (bytes.size() < length_size) Fail(error, "Truncated AVC NAL length");
    std::uint32_t length = 0;
    for (auto b : bytes.first(length_size)) length = (length << 8) | std::to_integer<unsigned>(b);
    bytes = bytes.subspan(length_size);
    if (!length || length > bytes.size()) Fail(error, "Invalid AVC NAL length");
    const auto header = std::to_integer<unsigned>(bytes.front());
    if (header & 128) Fail(error, "Invalid AVC NAL header");
    const auto type = header & 31;
    picture |= type >= 1 && type <= 5;
    idr |= type == 5;
    bytes = bytes.subspan(length);
  }
  if (!picture) Fail(error, "AVC sample has no coded picture");
  return idr;
}

void ValidateMp4(std::span<const MediaTrack> tracks, ContainerLayout layout) {
  MovieScale(tracks);
  for (const auto& track : tracks) {
    if (track.id == 0xffffffffu) Fail(WriteError, "MP4 track ID leaves no next track ID");
    if (track.kind == TrackKind::Audio && track.codec == CodecId::Aac && track.framing == PacketFraming::CodecPacket) {
      const auto config = ParseAac(track.initialization_data);
      if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
      if (track.sample_rate != config.Value().sample_rate || track.channel_count != config.Value().channels ||
          *track.sample_rate > 65535)
        Fail(WriteError, "MP4 AAC track metadata does not match AudioSpecificConfig");
      if (layout == ContainerLayout::Fragmented &&
          (track.priming_samples.value_or(0) || track.padding_samples.value_or(0)))
        Fail(ContainerErrorCode::Unsupported, "Fragmented MP4 cannot retain audio trim in this layout");
    } else if (track.kind == TrackKind::Video && track.codec == CodecId::H264 &&
               track.framing == PacketFraming::LengthPrefixedNals) {
      const auto config = ParseAvc(track.initialization_data);
      if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
      const auto width = track.coded_width.value_or(0), height = track.coded_height.value_or(0);
      if (!width || !height || width > 65535 || height > 65535 || track.priming_samples || track.padding_samples)
        Fail(WriteError, "Invalid MP4 AVC track dimensions or trim");
      if (track.visible && (!track.visible->width || !track.visible->height || track.visible->x >= width ||
          track.visible->y >= height || track.visible->width > width - track.visible->x ||
          track.visible->height > height - track.visible->y))
        Fail(WriteError, "MP4 visible rectangle exceeds coded bounds");
    } else {
      Fail(ContainerErrorCode::Unsupported, "MP4 supports raw AAC-LC and length-prefixed AVC tracks");
    }
  }
}

std::unique_ptr<ContainerWriter> OpenMp4Writer(ContainerOutput output, std::span<const MediaTrack> tracks,
                                               ContainerLayout layout) {
  return std::make_unique<Mp4Writer>(std::move(output), tracks, layout);
}
} // namespace huxerui::media::detail
