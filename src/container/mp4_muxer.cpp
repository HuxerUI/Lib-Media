#include "mp4.h"
#include "framed_audio.h"
#include "pcm.h"

#include "codec/common.h"

#include <array>
#include <charconv>
#include <numeric>

namespace huxerui::media::detail {
namespace {

constexpr auto WriteError = ContainerErrorCode::InvalidArgument;

void Zero(Bytes& bytes, std::size_t size) { bytes.resize(bytes.size() + size); }

std::pair<std::uint16_t, std::uint16_t> TrackNumber(std::string_view text) {
  if (text.empty()) return {};
  const auto slash = text.find('/');
  auto read = [](std::string_view field) {
    std::uint16_t number = 0;
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !number)
      Fail(WriteError, "MP4 track number must be a positive 16-bit index with an optional total");
    return number;
  };
  return {read(text.substr(0, slash)), slash == std::string_view::npos ? 0 : read(text.substr(slash + 1))};
}

void AudioMetadata(Bytes& bytes, const MediaAudioMetadata& metadata) {
  if (!HasAudioMetadata(metadata)) return;
  Box(bytes, "udta", [&] {
    FullBox(bytes, "meta", 0, [&] {
      FullBox(bytes, "hdlr", 0, [&] {
        Put(bytes, 0u);
        Put(bytes, Tag("mdir"));
        Put(bytes, Tag("appl"));
        Zero(bytes, 9);
      });
      Box(bytes, "ilst", [&] {
        auto item = [&](std::string_view tag, unsigned type, auto payload) {
          Box(bytes, tag, [&] {
            FullBox(bytes, "data", type, [&] { Put(bytes, 0u); payload(); });
          });
        };
        for (const auto& tag : Mp4AudioTags) {
          const auto& text = metadata.*tag.field;
          if (!text.empty()) item(tag.name, 1, [&] {
            Append(bytes, std::as_bytes(std::span{text.data(), text.size()}));
          });
        }
        const auto [number, total] = TrackNumber(metadata.track_number);
        if (number) item("trkn", 0, [&] {
          Put<std::uint16_t>(bytes, 0);
          Put(bytes, number);
          Put(bytes, total);
          Put<std::uint16_t>(bytes, 0);
        });
        if (const auto& cover = metadata.cover)
          item("covr", cover->mime_type == "image/jpeg" ? 13 : 14, [&] { Append(bytes, cover->data.AsBytes()); });
      });
    });
  });
}

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
  const auto include = [&](MediaTimestamp time) {
    if (!time.timescale || time.ticks < 0) Fail(WriteError, "Invalid MP4 presentation time");
    const auto divisor = std::gcd(std::uint64_t(time.ticks), std::uint64_t(time.timescale));
    scale = CommonScale(scale, static_cast<std::uint32_t>(time.timescale / divisor));
  };
  for (const auto& track : tracks) {
    if (track.edits.size() > MaxMp4Edits) Fail(ContainerErrorCode::LimitExceeded, "MP4 edit list exceeds its limit");
    scale = CommonScale(scale, Mp4Scale(track));
    for (const auto& edit : track.edits) include(edit.duration);
  }
  return scale;
}

std::uint32_t DisplayWidth(const MediaSampleDescription& track, std::uint32_t width) {
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

std::string_view VideoEntry(const MediaSampleDescription& track) {
  if (!track.sample_entry.empty()) return track.sample_entry;
  if (track.codec == CodecId::Av1) return "av01";
  return track.codec == CodecId::Hevc ? "hvc1" : "avc1";
}

std::uint32_t PcmFrameSize(const MediaSampleDescription& track) {
  return (DescribePackedPcm(track.codec).bits / 8) * track.channel_count.value_or(0);
}

void PcmDescription(Bytes& bytes, const MediaSampleDescription& track) {
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
    const auto pcm = DescribePackedPcm(track.codec);
    const auto flags = 8u | (pcm.floating_point ? 1u : (pcm.bits > 8 ? 4u : 0u)) | (pcm.big_endian ? 2u : 0u);
    Put(bytes, 72u);
    Put(bytes, std::bit_cast<std::uint64_t>(static_cast<double>(*track.sample_rate)));
    Put(bytes, *track.channel_count);
    Put(bytes, 0x7f000000u);
    Put(bytes, pcm.bits);
    Put(bytes, flags);
    Put(bytes, PcmFrameSize(track));
    Put(bytes, 1u);
    if (const auto layout = WritePcmChannelLayout(track); !layout.empty())
      FullBox(bytes, "chan", 0, [&] { Append(bytes, layout); });
  });
}

void SampleDescription(Bytes& bytes, TrackKind kind, const MediaSampleDescription& track, bool mov) {
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
  } else if (track.codec == CodecId::Alac) {
    const auto alac = ParseAlac(track.initialization_data, WriteError);
    Box(bytes, "alac", [&] {
      Zero(bytes, 6);
      Put<std::uint16_t>(bytes, 1);
      Zero(bytes, 8);
      Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(alac.channels));
      Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(alac.bits));
      Zero(bytes, 4);
      Put(bytes, alac.sample_rate <= 65535 ? alac.sample_rate << 16 : 0);
      FullBox(bytes, "alac", 0, [&] { Append(bytes, track.initialization_data); });
    });
  } else if (kind == TrackKind::Audio) {
    PcmDescription(bytes, track);
  } else if (kind == TrackKind::Data) {
    Box(bytes, track.sample_entry, [&] {
      Zero(bytes, 6);
      Put<std::uint16_t>(bytes, 1);
      Append(bytes, track.initialization_data);
    });
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
      else if (track.codec == CodecId::Av1)
        Box(bytes, "av1C", [&] { Append(bytes, track.initialization_data); });
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
}

void SampleTable(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples, bool mov) {
  const auto& descriptions = track.descriptions;
  const auto& primary = descriptions.front();
  Box(bytes, "stbl", [&] {
    FullBox(bytes, "stsd", 0, [&] {
      Put(bytes, static_cast<std::uint32_t>(descriptions.size()));
      for (const auto& description : descriptions) SampleDescription(bytes, track.kind, description, mov);
    });
    const auto frame_bytes = PcmFrameSize(primary);
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
          Put(bytes, Mp4Scale(track) / *primary.sample_rate);
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
      std::uint32_t runs = 0, previous = 0, description = 0;
      for (std::uint32_t i = 0; i < chunk_count; ++i) {
        const auto frames = frame_bytes ? samples[i].size / frame_bytes : 1u;
        if (frames == previous && description == samples[i].description) continue;
        description = samples[i].description;
        Put(bytes, i + 1);
        Put(bytes, frames);
        Put(bytes, description);
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

struct TrackTiming {
  std::vector<MediaTrack::Edit> edits;
  std::int64_t duration = 0;
  std::int64_t media_duration = 0;
};

std::int64_t AudioTrim(const MediaTrack& track, std::uint64_t samples) {
  if (!samples) return 0;
  const auto rate = track.descriptions.front().sample_rate;
  if (!rate || samples > INT64_MAX) Fail(WriteError, "Invalid MP4 audio trim");
  return ExactTicks({static_cast<std::int64_t>(samples), *rate}, Mp4Scale(track));
}

TrackTiming Timing(const MediaTrack& track, std::span<const Mp4Sample> samples, std::uint32_t movie_scale,
                   std::optional<std::int64_t> first_pts, std::optional<std::int64_t> end_pts) {
  const auto scale = Mp4Scale(track);
  const auto origin = samples.empty() ? 0 : samples.front().dts;
  TrackTiming timing;
  if (!samples.empty())
    timing.media_duration = SubTime(AddTime(samples.back().dts, samples.back().duration), origin);
  if (!track.edits.empty()) {
    timing.edits = track.edits;
    for (auto& edit : timing.edits) {
      timing.duration = AddTime(timing.duration, ExactTicks(edit.duration, movie_scale));
      if (edit.media_start) {
        const auto rebased = SubTime(ExactTicks(*edit.media_start, scale), origin);
        if (rebased < 0) Fail(WriteError, "MP4 edit starts before the track's decode origin");
        edit.media_start = MediaTimestamp{rebased, scale};
      }
    }
  } else if (first_pts) {
    const auto start = std::max<std::int64_t>(0, AddTime(*first_pts, AudioTrim(track, track.priming_samples.value_or(0))));
    const auto end = SubTime(*end_pts, AudioTrim(track, track.padding_samples.value_or(0)));
    const auto media_start = SubTime(start, origin);
    if (end <= start || media_start < 0)
      Fail(ContainerErrorCode::Unsupported, "MP4 edit cannot represent the requested presentation interval");
    timing.duration = ExactTicks({end, scale}, movie_scale);
    if (start) timing.edits.push_back({{start, scale}, {}});
    if (start || media_start || end != *end_pts)
      timing.edits.push_back({{SubTime(end, start), scale}, MediaTimestamp{media_start, scale}});
  }
  return timing;
}

void TrackBox(Bytes& bytes, const MediaTrack& track, std::span<const Mp4Sample> samples,
              const TrackTiming& timing, std::uint32_t movie_scale, bool mov) {
  const auto& primary = track.descriptions.front();
  const bool video = track.kind == TrackKind::Video;
  const bool data = track.kind == TrackKind::Data;
  Box(bytes, "trak", [&] {
    FullBox(bytes, "tkhd", 0x01000007, [&] {
      Zero(bytes, 16);
      Put(bytes, track.id);
      Put(bytes, 0u);
      Put(bytes, timing.duration);
      Zero(bytes, 12);
      Put<std::uint16_t>(bytes, track.kind == TrackKind::Audio ? 0x100 : 0);
      Put<std::uint16_t>(bytes, 0);
      const auto width = video ? DisplayWidth(primary, primary.visible ? primary.visible->width : *primary.coded_width) : 0;
      const auto height = (primary.visible ? primary.visible->height : primary.coded_height.value_or(0)) << 16;
      Matrix(bytes, track.rotation, width, height);
      Put(bytes, width);
      Put(bytes, height);
    });
    if (!track.references.empty()) Box(bytes, "tref", [&] {
      for (const auto type : {"tmcd", "cdsc"}) {
        if (std::ranges::none_of(track.references, [&](const auto& ref) { return ref.type == type; })) continue;
        Box(bytes, type, [&] {
          for (const auto& ref : track.references) if (ref.type == type) Put(bytes, ref.track_id);
        });
      }
    });
    if (!timing.edits.empty()) Box(bytes, "edts", [&] {
      FullBox(bytes, "elst", 0x01000000, [&] {
        Put(bytes, static_cast<std::uint32_t>(timing.edits.size()));
        for (const auto& edit : timing.edits) {
          Put(bytes, ExactTicks(edit.duration, movie_scale));
          Put(bytes, edit.media_start ? ExactTicks(*edit.media_start, Mp4Scale(track)) : -1);
          Put(bytes, 0x10000u);
        }
      });
    });
    Box(bytes, "mdia", [&] {
      FullBox(bytes, "mdhd", 0x01000000, [&] {
        Zero(bytes, 16);
        Put(bytes, Mp4Scale(track));
        Put(bytes, timing.media_duration);
        Put(bytes, 0x55c40000u);
      });
      FullBox(bytes, "hdlr", 0, [&] {
        Put(bytes, mov ? Tag("mhlr") : 0u);
        Put(bytes, Tag(video ? "vide" : !data ? "soun" : primary.sample_entry == "tmcd" ? "tmcd" : "meta"));
        Zero(bytes, 13);
      });
      Box(bytes, "minf", [&] {
        if (data) {
          Box(bytes, "gmhd", [&] {
            FullBox(bytes, "gmin", 0, [&] {
              Put<std::uint16_t>(bytes, 0x40);
              for (int i = 0; i < 3; ++i) Put<std::uint16_t>(bytes, 0x8000);
              Zero(bytes, 4);
            });
          });
        } else {
          FullBox(bytes, video ? "vmhd" : "smhd", video ? 1 : 0, [&] { Zero(bytes, video ? 8 : 4); });
        }
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
  Mp4Writer(ContainerOutput output, ContainerFormat format, std::span<const MediaTrack> tracks,
            const MediaMuxerOptions& options)
      : ContainerWriter(std::move(output), tracks), mov_(format == ContainerFormat::Mov),
        fragmented_(options.layout == ContainerLayout::Fragmented), fast_start_(options.layout == ContainerLayout::FastStart),
        movie_scale_(MovieScale(tracks)), audio_metadata_(options.audio_metadata), states_(tracks.size()) {
    Bytes header;
    const bool audio_only = tracks.size() == 1 && tracks.front().kind == TrackKind::Audio;
    Box(header, "ftyp", [&] {
      Put(header, Tag(mov_ ? "qt  " : fragmented_ ? "iso6" : audio_only ? "M4A " : "isom"));
      Put(header, 512u);
      if (mov_) {
        Put(header, Tag("qt  "));
      } else {
        for (auto brand : {"isom", "iso6", "mp41"}) Put(header, Tag(brand));
        if (audio_only && !fragmented_) Put(header, Tag("M4A "));
        if (std::ranges::any_of(tracks, [](const auto& track) { return track.descriptions.front().codec == CodecId::H264; }))
          Put(header, Tag("avc1"));
        if (std::ranges::any_of(tracks, [](const auto& track) { return track.descriptions.front().codec == CodecId::Av1; }))
          Put(header, Tag("av01"));
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
    const auto& description = track.descriptions[packet.sample_description_index - 1];
    const auto scale = Mp4Scale(track);
    const auto frame_bytes = PcmFrameSize(description);
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
      derived_duration = MediaTimestamp{frames, *description.sample_rate};
      if (packet.duration && *packet.duration != *derived_duration)
        Fail(WriteError, "QuickTime PCM packet duration does not match its frame count");
      if (!pts) pts = dts.value_or(MediaTimestamp{state.next_dts.value_or(0), scale});
      if (!dts) dts = pts;
      if (pts != dts || packet.sync_sample == false)
        Fail(WriteError, "QuickTime PCM requires one presentation/decode time and independent samples");
    } else if (description.codec == CodecId::Aac) {
      if (!dts) dts = pts;
      derived_duration = MediaTimestamp{1024, *description.sample_rate};
    } else if (description.codec == CodecId::Alac) {
      if (!dts) dts = pts;
      const auto alac = ParseAlac(description.initialization_data, WriteError);
      if (packet.duration) {
        const auto samples = ExactTicks(*packet.duration, alac.sample_rate);
        if (samples <= 0 || samples > alac.frame_length)
          Fail(WriteError, "ALAC packet duration exceeds its configured frame length");
      }
      if (pts != dts || packet.sync_sample == false)
        Fail(WriteError, "ALAC requires independent samples and matching PTS/DTS");
    } else if (description.codec == CodecId::ProRes && !dts) {
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
    const auto presentation_end = AddTime(pts_ticks, length);
    bool independent = true;
    if (description.codec == CodecId::H264 || description.codec == CodecId::Hevc) {
      const bool avc = description.codec == CodecId::H264;
      const auto length = (Number<std::uint8_t>(description.initialization_data, avc ? 4 : 21) & 3) + 1;
      independent = avc ? AvcIdr(data, length, WriteError)
                        : HevcIdr(data, length, VideoEntry(description) == "hvc1", WriteError);
    } else if (description.codec == CodecId::Av1) {
      independent = Av1Sync(data, WriteError);
      if (packet.sync_sample == true && !independent) Fail(WriteError, "AV1 sync sample has no random-access key frame");
    }
    if (track.kind == TrackKind::Data && description.sample_entry == "tmcd" && data.size() != 4)
      Fail(WriteError, "QuickTime timecode requires four-byte samples");
    if (description.codec == CodecId::ProRes &&
        (data.size() < 28 || Number<std::uint32_t>(data) != data.size() || Number<std::uint32_t>(data, 4) != Tag("icpf")))
      Fail(WriteError, "Invalid ProRes frame header or size");
    const bool sync = packet.sync_sample.value_or(independent);
    state.bytes.assign(data.begin(), data.end());
    state.pending = Mp4Sample{0, static_cast<std::uint32_t>(data.size()), pts_ticks, dts_ticks,
                             static_cast<std::uint32_t>(length), sync, packet.sample_description_index};
    state.first_pts = std::min(state.first_pts.value_or(pts_ticks), pts_ticks);
    state.end_pts = std::max(state.end_pts.value_or(presentation_end), presentation_end);
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
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto& track = tracks[i];
      const auto& state = states_[i];
      if (!fragmented_ && !track.edits.empty() && state.index.empty())
        Fail(WriteError, "A presentation edit requires media samples");
      if (track.edits.empty() || (!track.priming_samples && !track.padding_samples)) continue;
      const auto media = std::ranges::find_if(track.edits, [](const auto& edit) { return edit.media_start.has_value(); });
      if (std::ranges::any_of(std::ranges::subrange(std::next(media), track.edits.end()),
                             [](const auto& edit) { return edit.media_start.has_value(); }))
        Fail(WriteError, "MP4 multi-segment edits already describe audio trim");
      const auto scale = Mp4Scale(track);
      const auto start = ExactTicks(*media->media_start, scale);
      if (!state.first_pts ||
          (track.priming_samples && start != AddTime(*state.first_pts, AudioTrim(track, *track.priming_samples))) ||
          (track.padding_samples && AddTime(start, ExactTicks(media->duration, scale)) !=
              SubTime(*state.end_pts, AudioTrim(track, *track.padding_samples))))
        Fail(WriteError, "MP4 audio trim conflicts with the declared presentation edits");
    }
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
    std::uint32_t pcm_frames = 0;
    std::optional<std::int64_t> next_dts;
    std::optional<std::int64_t> first_pts, end_pts;
    std::optional<Mp4Sample> pending;
    Bytes bytes;
    std::vector<Mp4Sample> index;
  };

  Bytes Movie() const {
    std::vector<TrackTiming> timings;
    std::int64_t duration = 0;
    std::uint32_t max_id = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto& state = states_[i];
      auto timing = Timing(tracks[i], state.index, movie_scale_, state.first_pts, state.end_pts);
      duration = std::max(duration, timing.duration);
      timings.push_back(std::move(timing));
      max_id = std::max(max_id, tracks[i].id);
    }
    Bytes bytes;
    Box(bytes, "moov", [&] {
      FullBox(bytes, "mvhd", 0x01000000, [&] {
        Zero(bytes, 16);
        Put(bytes, movie_scale_);
        Put(bytes, fragmented_ ? 0 : duration);
        Put(bytes, 0x10000u);
        Put<std::uint16_t>(bytes, 0x100);
        Zero(bytes, 10);
        Matrix(bytes);
        Zero(bytes, 24);
        Put(bytes, max_id + 1);
      });
      for (std::size_t i = 0; i < tracks.size(); ++i)
        TrackBox(bytes, tracks[i], states_[i].index, timings[i], movie_scale_, mov_);
      if (audio_metadata_) AudioMetadata(bytes, *audio_metadata_);
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
        FullBox(bytes, "tfhd", sample.description == 1 ? 0x20000 : 0x20002, [&] {
          Put(bytes, id);
          if (sample.description != 1) Put(bytes, sample.description);
        });
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
  std::optional<MediaAudioMetadata> audio_metadata_;
  std::vector<TrackState> states_;
  std::size_t samples_ = 0;
  std::uint32_t sequence_ = 0;
  std::uint64_t mdat_ = 0;
};
} // namespace

std::uint32_t Mp4Scale(const MediaTrack& track) {
  return track.timescale.value_or(track.descriptions.front().sample_rate.value_or(90000));
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

void ValidateMp4(ContainerFormat format, std::span<const MediaTrack> tracks, const MediaMuxerOptions& options) {
  const auto movie_scale = MovieScale(tracks);
  if (options.audio_metadata) TrackNumber(options.audio_metadata->track_number);
  for (const auto& track : tracks) {
    if (track.id == 0xffffffffu) Fail(WriteError, "MP4 track ID leaves no next track ID");
    const auto& primary = track.descriptions.front();
    if (track.descriptions.size() > 1 &&
        (primary.codec != CodecId::H264 && primary.codec != CodecId::Hevc && primary.codec != CodecId::Av1 && primary.codec != CodecId::Aac))
      Fail(WriteError, "This MP4 codec requires one sample description");
    for (const auto& ref : track.references) {
      const auto target = std::ranges::find(tracks, ref.track_id, &MediaTrack::id);
      if (target == tracks.end() || target->id == track.id ||
          !((ref.type == "tmcd" && (track.kind == TrackKind::Video || track.kind == TrackKind::Audio) &&
             target->descriptions.front().sample_entry == "tmcd") ||
            (ref.type == "cdsc" && primary.sample_entry == "mebx" &&
             (target->kind == TrackKind::Audio || target->kind == TrackKind::Video))))
        Fail(WriteError, "Invalid QuickTime track association");
    }
    if (!track.edits.empty()) {
      const auto timeline = Mp4Edits(track.edits, movie_scale, Mp4Scale(track), WriteError);
      if (timeline.segments.size() != 1 && (track.priming_samples || track.padding_samples))
        Fail(WriteError, "MP4 multi-segment edits already describe audio trim");
    }
    for (const auto& description : track.descriptions) {
      if (description.codec != primary.codec || description.framing != primary.framing ||
          description.sample_rate != primary.sample_rate || description.channel_count != primary.channel_count)
        Fail(WriteError, "MP4 sample descriptions must share a codec and audio geometry");
      if (track.kind == TrackKind::Audio && description.codec == CodecId::Aac && description.framing == PacketFraming::CodecPacket) {
        const auto config = ParseAac(description.initialization_data);
        if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
        if (description.sample_rate != config.Value().sample_rate || description.channel_count != config.Value().channels ||
            *description.sample_rate > 65535 || (!description.sample_entry.empty() && description.sample_entry != "mp4a"))
          Fail(WriteError, "MP4 AAC track metadata does not match AudioSpecificConfig");
        if (options.layout == ContainerLayout::Fragmented) {
          if (track.edits.empty() && (track.priming_samples.value_or(0) || track.padding_samples.value_or(0)))
            Fail(ContainerErrorCode::Unsupported, "Fragmented MP4 audio trim requires a presentation interval at open");
          if (track.priming_samples.value_or(0) > INT64_MAX || track.padding_samples.value_or(0) > INT64_MAX)
            Fail(WriteError, "MP4 audio trim is outside its clock");
        }
      } else if (track.kind == TrackKind::Audio && format == ContainerFormat::Mp4 && description.codec == CodecId::Alac) {
        const auto alac = ParseAlac(description.initialization_data, WriteError);
        if (description.sample_rate != alac.sample_rate || description.channel_count != alac.channels ||
            Mp4Scale(track) % alac.sample_rate || description.framing != PacketFraming::CodecPacket ||
            (!description.sample_entry.empty() && description.sample_entry != "alac"))
          Fail(WriteError, "MP4 ALAC track metadata does not match ALACSpecificConfig");
        if (options.layout == ContainerLayout::Fragmented)
          Fail(ContainerErrorCode::Unsupported, "ALAC output requires regular or fast-start MP4");
      } else if (track.kind == TrackKind::Audio && format == ContainerFormat::Mov && DescribePackedPcm(description.codec).bits) {
        const auto rate = description.sample_rate.value_or(0), channels = description.channel_count.value_or(0);
        if (!rate || !channels || channels > 64 || Mp4Scale(track) % rate ||
            description.framing != PacketFraming::CodecPacket || !description.sample_entry.empty())
          Fail(WriteError, "Invalid QuickTime PCM format or sample clock");
        ValidatePcmChannelLayout(description);
      } else if (track.kind == TrackKind::Data && format == ContainerFormat::Mov &&
                 description.codec == CodecId::Unknown && description.framing == PacketFraming::ContainerSample) {
        if (description.sample_rate || description.channel_count || track.priming_samples || track.padding_samples ||
            description.coded_width || description.coded_height || track.rotation != VideoRotation::None)
          Fail(WriteError, "Invalid QuickTime data track description");
        ValidateMovDataDescription(description.sample_entry, description.initialization_data, WriteError);
      } else if (track.kind == TrackKind::Video) {
        if (description.codec == CodecId::H264 && description.framing == PacketFraming::LengthPrefixedNals && VideoEntry(description) == "avc1") {
          const auto config = ParseAvc(description.initialization_data);
          if (!config.Succeeded()) Fail(ContainerErrorCode::Unsupported, config.Error().message);
        } else if (description.codec == CodecId::Hevc && description.framing == PacketFraming::LengthPrefixedNals &&
                   (VideoEntry(description) == "hvc1" || VideoEntry(description) == "hev1")) {
          HevcNalLength(description.initialization_data, VideoEntry(description) == "hvc1", WriteError);
        } else if (description.codec == CodecId::Av1 && format == ContainerFormat::Mp4 &&
                   description.framing == PacketFraming::CodecPacket && VideoEntry(description) == "av01") {
          ValidateAv1Configuration(description.initialization_data, WriteError);
        } else if (description.codec != CodecId::ProRes || format != ContainerFormat::Mov ||
                   description.framing != PacketFraming::CodecPacket || description.sample_entry.size() != 4 ||
                   !ProResEntry(Tag(description.sample_entry)) || !description.initialization_data.empty()) {
          Fail(ContainerErrorCode::Unsupported, "Unsupported MP4 or QuickTime video sample entry");
        }
        const auto width = description.coded_width.value_or(0), height = description.coded_height.value_or(0);
        if (!width || !height || width > 65535 || height > 65535 || track.priming_samples || track.padding_samples)
          Fail(WriteError, "Invalid video track dimensions or trim");
        if (description.visible && (!description.visible->width || !description.visible->height || description.visible->x >= width ||
            description.visible->y >= height || description.visible->width > width - description.visible->x ||
            description.visible->height > height - description.visible->y))
          Fail(WriteError, "Visible rectangle exceeds coded bounds");
        if (const auto& aspect = description.pixel_aspect_ratio; aspect && (!aspect->width || !aspect->height))
          Fail(WriteError, "MP4 pixel aspect terms must be positive");
        const auto display_width = DisplayWidth(description, description.visible ? description.visible->width : width);
        if (track.rotation < VideoRotation::None || track.rotation > VideoRotation::Clockwise270 ||
            (track.rotation != VideoRotation::None &&
             (display_width > std::numeric_limits<std::int32_t>::max() ||
              (description.visible ? description.visible->height : height) > 32767)))
          Fail(WriteError, "Video rotation cannot be represented by the track matrix");
        for (auto extension = description.video_extensions.begin(); extension != description.video_extensions.end(); ++extension) {
          if (extension->type.size() != 4 ||
              std::ranges::find(description.video_extensions.begin(), extension, extension->type,
                                &MediaSampleDescription::VideoExtension::type) != extension)
            Fail(WriteError, "Invalid or duplicate MP4 visual extension type");
          ValidateMp4VideoExtension(Tag(extension->type), extension->payload, description.codec, WriteError);
        }
      } else {
        Fail(ContainerErrorCode::Unsupported, "Unsupported MP4 or QuickTime audio sample entry");
      }
    }
    if (track.kind != TrackKind::Video && track.rotation != VideoRotation::None)
      Fail(WriteError, "Presentation rotation requires a video track");
  }
}

std::unique_ptr<ContainerWriter> OpenMp4Writer(ContainerOutput output, ContainerFormat format,
                                               std::span<const MediaTrack> tracks, const MediaMuxerOptions& options) {
  return std::make_unique<Mp4Writer>(std::move(output), format, tracks, options);
}
} // namespace huxerui::media::detail
