#include "mp4.h"

#include "codec/common.h"

#include <array>
#include <numeric>

namespace huxerui::media::detail {
namespace {

constexpr auto WriteError = ContainerErrorCode::InvalidArgument;

void Zero(Bytes& bytes, std::size_t size) { bytes.resize(bytes.size() + size); }

void Matrix(Bytes& bytes, VideoRotation rotation = VideoRotation::None,
            std::uint32_t width = 0, std::uint32_t height = 0) {
  constexpr std::int32_t unit = 0x10000;
  const auto x = static_cast<std::int32_t>(width);
  const auto y = static_cast<std::int32_t>(height);
  std::array<std::int32_t, 9> matrix{unit, 0, 0, 0, unit, 0, 0, 0, 0x40000000};
  // QuickTime stores a row-vector transform. Translate the rotated rectangle back to a zero origin.
  switch (rotation) {
    case VideoRotation::Clockwise90: matrix = {0, unit, 0, -unit, 0, 0, y, 0, 0x40000000}; break;
    case VideoRotation::Clockwise180: matrix = {-unit, 0, 0, 0, -unit, 0, x, y, 0x40000000}; break;
    case VideoRotation::Clockwise270: matrix = {0, -unit, 0, unit, 0, 0, 0, x, 0x40000000}; break;
    case VideoRotation::None: break;
  }
  for (auto value : matrix) Put(bytes, value);
}

std::uint32_t MovieScale(std::span<const MediaTrack> tracks) {
  std::uint32_t scale = 1;
  for (const auto& track : tracks) {
    scale = CommonScale(scale, Mp4Scale(track));
    if (const auto& range = track.presentation_edit) {
      for (const auto time : {range->start, range->end}) {
        const auto divisor = std::gcd(std::uint64_t(time.ticks), std::uint64_t(time.timescale));
        scale = CommonScale(scale, static_cast<std::uint32_t>(time.timescale / divisor));
      }
    }
  }
  return scale;
}

std::uint32_t DisplayWidth(const MediaTrack& track, std::uint32_t width) {
  auto fixed = std::uint64_t(width) << 16;
  if (const auto& aspect = track.pixel_aspect_ratio)
    fixed = (fixed * aspect->width + aspect->height / 2) / aspect->height;
  if (!fixed || fixed > std::numeric_limits<std::uint32_t>::max())
    Fail(WriteError, "MP4 display width exceeds the track header grid");
  return static_cast<std::uint32_t>(fixed);
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

std::string_view VideoEntry(const MediaTrack& track) {
  if (!track.sample_entry.empty()) return track.sample_entry;
  return track.codec == CodecId::Hevc ? "hvc1" : "avc1";
}

std::uint32_t PcmFrameSize(const MediaTrack& track) {
  return (MovPcmBits(track.codec) / 8) * track.channel_count.value_or(0);
}

void PcmDescription(Bytes& bytes, const MediaTrack& track) {
  Box(bytes, "lpcm", [&] {
    Zero(bytes, 6);
    Put<std::uint16_t>(bytes, 1);
    Put<std::uint16_t>(bytes, 2);
    Zero(bytes, 6);
    Put<std::uint16_t>(bytes, 3);
    Put<std::uint16_t>(bytes, 16);
    Put<std::uint16_t>(bytes, 0xfffe);
    Put<std::uint16_t>(bytes, 0);
    Put(bytes, 0x10000u);
    // The version-two fields describe one packed, interleaved PCM frame per QuickTime sample.
    const auto bits = MovPcmBits(track.codec);
    const bool floating = track.codec == CodecId::PcmF32Le || track.codec == CodecId::PcmF32Be;
    const auto flags = 8u | (floating ? 1u : (bits > 8 ? 4u : 0u)) | (MovPcmBigEndian(track.codec) ? 2u : 0u);
    Put(bytes, 72u);
    Put(bytes, std::bit_cast<std::uint64_t>(static_cast<double>(*track.sample_rate)));
    Put(bytes, *track.channel_count);
    Put(bytes, 0x7f000000u);
    Put(bytes, bits);
    Put(bytes, flags);
    Put(bytes, PcmFrameSize(track));
    Put(bytes, 1u);
  });
}

void SampleDescription(Bytes& bytes, const MediaTrack& track, bool mov) {
  FullBox(bytes, "stsd", 0, [&] {
    Put(bytes, 1u);
    if (track.codec == CodecId::Aac) {
      Box(bytes, "mp4a", [&] {
        Zero(bytes, 6);
        Put<std::uint16_t>(bytes, 1);
        Put<std::uint16_t>(bytes, mov ? 1 : 0);
        Zero(bytes, 6);
        Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(*track.channel_count));
        Put<std::uint16_t>(bytes, 16);
        Put<std::uint16_t>(bytes, mov ? 0xfffe : 0);
        Put<std::uint16_t>(bytes, 0);
        Put(bytes, *track.sample_rate << 16);
        const auto descriptor = [&] {
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
        };
        if (mov) {
          Put(bytes, 1024u);
          Zero(bytes, 8);
          Put(bytes, 2u);
          // QuickTime stores MPEG-4 audio decoder parameters inside the version-one wave extension.
          Box(bytes, "wave", [&] {
            Box(bytes, "frma", [&] { Put(bytes, Tag("mp4a")); });
            Box(bytes, "mp4a", [&] { Put(bytes, 0u); });
            descriptor();
            Put(bytes, 8u);
            Put(bytes, 0u);
          });
        } else {
          descriptor();
        }
      });
    } else if (track.kind == TrackKind::Audio) {
      PcmDescription(bytes, track);
    } else {
      Box(bytes, VideoEntry(track), [&] {
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
        if (track.codec == CodecId::H264 || track.codec == CodecId::Hevc)
          Box(bytes, track.codec == CodecId::Hevc ? "hvcC" : "avcC", [&] { Append(bytes, track.initialization_data); });
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
        if (const auto& aspect = track.pixel_aspect_ratio) {
          Box(bytes, "pasp", [&] { Put(bytes, aspect->width); Put(bytes, aspect->height); });
        }
        for (const auto& extension : track.video_extensions)
          Box(bytes, extension.type, [&] { Append(bytes, extension.payload); });
      });
    }
  });
}

void SampleTable(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples, bool mov) {
  Box(bytes, "stbl", [&] {
    SampleDescription(bytes, track, mov);
    const auto frame_bytes = PcmFrameSize(track);
    const auto chunk_count = static_cast<std::uint32_t>(samples.size());
    std::uint32_t count = chunk_count;
    if (frame_bytes) {
      count = 0;
      for (const auto& sample : samples) count += sample.size / frame_bytes;
    }
    FullBox(bytes, "stts", 0, [&] {
      if (frame_bytes) {
        Put(bytes, count ? 1u : 0u);
        if (count) {
          Put(bytes, count);
          Put(bytes, Mp4Scale(track) / *track.sample_rate);
        }
        return;
      }
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
      const auto entries = bytes.size();
      Put(bytes, 0u);
      std::uint32_t runs = 0, previous = 0;
      for (std::uint32_t i = 0; i < chunk_count; ++i) {
        const auto frames = frame_bytes ? samples[i].size / frame_bytes : 1u;
        if (frames == previous) continue;
        Put(bytes, i + 1);
        Put(bytes, frames);
        Put(bytes, 1u);
        ++runs;
        previous = frames;
      }
      SetU32(bytes, entries, runs);
    });
    FullBox(bytes, "stsz", 0, [&] {
      Put(bytes, frame_bytes);
      Put(bytes, count);
      if (!frame_bytes) for (const auto& sample : samples) Put(bytes, sample.size);
    });
    FullBox(bytes, "co64", 0, [&] {
      Put(bytes, chunk_count);
      for (const auto& sample : samples) Put(bytes, sample.offset);
    });
    const auto sync_count = static_cast<std::uint32_t>(std::ranges::count(samples, true, &Mp4Sample::sync));
    if (!frame_bytes && count && (track.kind == TrackKind::Video || sync_count != count)) {
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
  if (samples.empty()) {
    if (track.presentation_edit) Fail(WriteError, "A presentation edit requires media samples");
    return {};
  }
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
  auto start = std::max<std::int64_t>(0, AddTime(first, trim(track.priming_samples.value_or(0))));
  end = SubTime(end, trim(track.padding_samples.value_or(0)));
  auto movie_end = ExactTicks({end, scale}, movie_scale);
  if (const auto& range = track.presentation_edit) {
    const auto explicit_start = ExactTicks(range->start, scale);
    if ((track.priming_samples && explicit_start != start) ||
        (track.padding_samples && range->end != MediaTimestamp{end, scale}))
      Fail(WriteError, "MP4 audio trim conflicts with its presentation interval");
    start = explicit_start;
    movie_end = ExactTicks(range->end, movie_scale);
  }
  const auto movie_start = ExactTicks({start, scale}, movie_scale);
  const auto media_start = SubTime(start, samples.front().dts);
  if (movie_end <= movie_start || media_start < 0)
    Fail(ContainerErrorCode::Unsupported, "MP4 edit cannot represent the requested presentation interval");
  return {movie_start, SubTime(movie_end, movie_start), media_start,
          SubTime(AddTime(samples.back().dts, samples.back().duration), samples.front().dts)};
}

void TrackBox(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples, const TrackEdit& edit, bool mov) {
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
      const auto width = video ? DisplayWidth(track, track.visible ? track.visible->width : *track.coded_width) : 0;
      const auto height = (track.visible ? track.visible->height : track.coded_height.value_or(0)) << 16;
      Matrix(bytes, track.rotation, width, height);
      Put(bytes, width);
      Put(bytes, height);
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
        Put(bytes, mov ? Tag("mhlr") : 0u);
        Put(bytes, Tag(video ? "vide" : "soun"));
        Zero(bytes, 13);
      });
      Box(bytes, "minf", [&] {
        FullBox(bytes, video ? "vmhd" : "smhd", video ? 1 : 0, [&] { Zero(bytes, video ? 8 : 4); });
        if (mov) {
          FullBox(bytes, "hdlr", 0, [&] {
            Put(bytes, Tag("dhlr"));
            Put(bytes, Tag("url "));
            Zero(bytes, 13);
          });
        }
        Box(bytes, "dinf", [&] {
          FullBox(bytes, "dref", 0, [&] {
            Put(bytes, 1u);
            FullBox(bytes, "url ", 1, [] {});
          });
        });
        SampleTable(bytes, track, samples, mov);
      });
    });
  });
}

class Mp4Writer final : public ContainerWriter {
public:
  Mp4Writer(ContainerOutput output, ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout)
      : ContainerWriter(std::move(output), tracks), mov_(format == ContainerFormat::Mov),
        fragmented_(layout == ContainerLayout::Fragmented), fast_start_(layout == ContainerLayout::FastStart),
        movie_scale_(MovieScale(tracks)), states_(tracks.size()) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].codec == CodecId::H264)
        states_[i].nal_length = ParseAvc(tracks[i].initialization_data).Value().length_size;
      if (tracks[i].codec == CodecId::Hevc)
        states_[i].nal_length = HevcNalLength(tracks[i].initialization_data, VideoEntry(tracks[i]) == "hvc1", WriteError);
    }
    Bytes header;
    Box(header, "ftyp", [&] {
      Put(header, Tag(mov_ ? "qt  " : (fragmented_ ? "iso6" : "isom")));
      Put(header, 512u);
      if (mov_) {
        Put(header, Tag("qt  "));
      } else {
        for (auto brand : {"isom", "iso6", "mp41"}) Put(header, Tag(brand));
        if (std::ranges::any_of(tracks, [](const auto& track) { return track.codec == CodecId::H264; }))
          Put(header, Tag("avc1"));
      }
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
    const auto frame_bytes = PcmFrameSize(track);
    const auto data = packet.data.AsBytes();
    std::uint32_t frames = 0;
    std::optional<MediaTimestamp> derived_duration;
    auto pts = packet.pts;
    auto dts = packet.dts;
    if (frame_bytes) {
      if (data.size() % frame_bytes) Fail(WriteError, "QuickTime PCM packet splits a sample frame");
      frames = static_cast<std::uint32_t>(data.size() / frame_bytes);
      if (frames > 0xffffffffu - state.pcm_frames)
        Fail(ContainerErrorCode::LimitExceeded, "QuickTime PCM track exceeds the sample table count");
      derived_duration = MediaTimestamp{frames, *track.sample_rate};
      if (packet.duration && *packet.duration != *derived_duration)
        Fail(WriteError, "QuickTime PCM packet duration does not match its frame count");
      if (!pts) pts = dts.value_or(MediaTimestamp{state.next_dts.value_or(0), scale});
      if (!dts) dts = pts;
      if (pts != dts || packet.sync_sample == false)
        Fail(WriteError, "QuickTime PCM requires one presentation/decode time and independent samples");
    } else if (track.codec == CodecId::Aac) {
      if (!dts) dts = pts;
      derived_duration = MediaTimestamp{1024, *track.sample_rate};
    } else if (track.codec == CodecId::ProRes && !dts) {
      dts = pts;
    }
    const auto duration = packet.duration ? packet.duration : derived_duration;
    if (!pts || !dts || !duration)
      Fail(ContainerErrorCode::MissingTiming, "MP4 requires PTS, decode timing, and a positive sample duration");
    const auto pts_ticks = ExactTicks(*pts, scale), dts_ticks = ExactTicks(*dts, scale);
    const auto length = ExactTicks(*duration, scale);
    const auto composition = SubTime(pts_ticks, dts_ticks);
    if (length <= 0 || length > 0xffffffffu || composition < std::numeric_limits<std::int32_t>::min() ||
        composition > std::numeric_limits<std::int32_t>::max())
      Fail(WriteError, "MP4 sample duration or composition offset is out of range");
    if ((state.next_dts && *state.next_dts != dts_ticks) || (fragmented_ && dts_ticks < 0))
      Fail(ContainerErrorCode::Unsupported, "MP4 requires a continuous decode grid and nonnegative fragment DTS");
    const auto next = AddTime(dts_ticks, length);
    AddTime(pts_ticks, length);
    bool independent = true;
    if (track.codec == CodecId::H264) independent = AvcIdr(data, state.nal_length, WriteError);
    if (track.codec == CodecId::Hevc)
      independent = HevcIdr(data, state.nal_length, VideoEntry(track) == "hvc1", WriteError);
    if (track.codec == CodecId::ProRes &&
        (data.size() < 28 || Number<std::uint32_t>(data) != data.size() || Number<std::uint32_t>(data, 4) != Tag("icpf")))
      Fail(WriteError, "Invalid ProRes frame header or size");
    const bool sync = packet.sync_sample.value_or(independent);
    state.bytes.assign(data.begin(), data.end());
    state.pending = Mp4Sample{0, static_cast<std::uint32_t>(data.size()), pts_ticks, dts_ticks,
                             static_cast<std::uint32_t>(length), sync};
    state.next_dts = next;
    state.pcm_frames += frames;
    ++samples_;
    Drain();
    return {};
  }

  void TrackEnded(std::uint32_t) override { Drain(); }

  void Finish() override {
    std::fill(ended.begin(), ended.end(), true);
    Drain();
    if (!fragmented_) {
      Bytes size;
      Put(size, output.Position() - mdat_);
      output.Patch(mdat_ + 8, size);
      if (fast_start_) {
        // co64 entries are fixed width, so relocating their offsets does not change the movie box size.
        const auto movie_size = Movie().size();
        for (auto& state : states_)
          for (auto& sample : state.index) sample.offset = AddOffset(sample.offset, movie_size);
        output.Insert(mdat_, Movie());
      } else {
        output.Write(Movie());
      }
    }
  }

private:
  struct TrackState {
    unsigned nal_length = 0;
    std::uint32_t pcm_frames = 0;
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
      for (std::size_t i = 0; i < tracks.size(); ++i) TrackBox(bytes, tracks[i], states_[i].index, edits[i], mov_);
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

  bool mov_;
  bool fragmented_;
  bool fast_start_;
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

void ValidateMp4(ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout) {
  for (const auto& track : tracks) {
    if (track.id == 0xffffffffu) Fail(WriteError, "MP4 track ID leaves no next track ID");
    if (const auto& range = track.presentation_edit) {
      if (layout == ContainerLayout::Fragmented)
        Fail(ContainerErrorCode::Unsupported, "Fragmented MP4 output cannot retain an explicit presentation interval");
      if (range->start.ticks < 0) Fail(WriteError, "MP4 presentation starts before the movie origin");
      ExactTicks(range->start, Mp4Scale(track));
    }
    if (track.kind == TrackKind::Audio && track.codec == CodecId::Aac && track.framing == PacketFraming::CodecPacket) {
      const auto config = ParseAac(track.initialization_data);
      if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
      if (track.sample_rate != config.Value().sample_rate || track.channel_count != config.Value().channels ||
          *track.sample_rate > 65535 || (!track.sample_entry.empty() && track.sample_entry != "mp4a"))
        Fail(WriteError, "MP4 AAC track metadata does not match AudioSpecificConfig");
      if (layout == ContainerLayout::Fragmented &&
          (track.priming_samples.value_or(0) || track.padding_samples.value_or(0)))
        Fail(ContainerErrorCode::Unsupported, "Fragmented MP4 cannot retain audio trim in this layout");
    } else if (track.kind == TrackKind::Audio && format == ContainerFormat::Mov && MovPcmBits(track.codec)) {
      const auto rate = track.sample_rate.value_or(0), channels = track.channel_count.value_or(0);
      if (!rate || !channels || channels > 64 || Mp4Scale(track) % rate ||
          track.framing != PacketFraming::CodecPacket || !track.initialization_data.empty() ||
          !track.sample_entry.empty())
        Fail(WriteError, "Invalid QuickTime PCM format or sample clock");
    } else if (track.kind == TrackKind::Video) {
      if (track.codec == CodecId::H264 && track.framing == PacketFraming::LengthPrefixedNals && VideoEntry(track) == "avc1") {
        const auto config = ParseAvc(track.initialization_data);
        if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
      } else if (track.codec == CodecId::Hevc && track.framing == PacketFraming::LengthPrefixedNals &&
                 (VideoEntry(track) == "hvc1" || VideoEntry(track) == "hev1")) {
        HevcNalLength(track.initialization_data, VideoEntry(track) == "hvc1", WriteError);
      } else if (track.codec != CodecId::ProRes || format != ContainerFormat::Mov ||
                 track.framing != PacketFraming::CodecPacket || track.sample_entry.size() != 4 ||
                 !ProResEntry(Tag(track.sample_entry)) || !track.initialization_data.empty()) {
        Fail(ContainerErrorCode::Unsupported, "Unsupported MP4 or QuickTime video sample entry");
      }
      const auto width = track.coded_width.value_or(0), height = track.coded_height.value_or(0);
      if (!width || !height || width > 65535 || height > 65535 || track.priming_samples || track.padding_samples)
        Fail(WriteError, "Invalid video track dimensions or trim");
      if (track.visible && (!track.visible->width || !track.visible->height || track.visible->x >= width ||
          track.visible->y >= height || track.visible->width > width - track.visible->x ||
          track.visible->height > height - track.visible->y))
        Fail(WriteError, "Visible rectangle exceeds coded bounds");
      if (const auto& aspect = track.pixel_aspect_ratio; aspect && (!aspect->width || !aspect->height))
        Fail(WriteError, "MP4 pixel aspect terms must be positive");
      const auto display_width = DisplayWidth(track, track.visible ? track.visible->width : width);
      if (track.rotation < VideoRotation::None || track.rotation > VideoRotation::Clockwise270 ||
          (track.rotation != VideoRotation::None &&
           (display_width > std::numeric_limits<std::int32_t>::max() ||
            (track.visible ? track.visible->height : height) > 32767)))
        Fail(WriteError, "Video rotation cannot be represented by the track matrix");
      for (auto extension = track.video_extensions.begin(); extension != track.video_extensions.end(); ++extension) {
        if (extension->type.size() != 4 ||
            std::ranges::find(track.video_extensions.begin(), extension, extension->type,
                              &MediaTrack::VideoExtension::type) != extension)
          Fail(WriteError, "Invalid or duplicate MP4 visual extension type");
        ValidateMp4VideoExtension(Tag(extension->type), extension->payload, track.codec, WriteError);
      }
    } else {
      Fail(ContainerErrorCode::Unsupported, "Unsupported MP4 or QuickTime audio sample entry");
    }
    if (track.kind != TrackKind::Video && track.rotation != VideoRotation::None)
      Fail(WriteError, "Presentation rotation requires a video track");
  }
  MovieScale(tracks);
}

std::unique_ptr<ContainerWriter> OpenMp4Writer(ContainerOutput output, ContainerFormat format,
                                               std::span<const MediaTrack> tracks, ContainerLayout layout) {
  return std::make_unique<Mp4Writer>(std::move(output), format, tracks, layout);
}
} // namespace huxerui::media::detail
