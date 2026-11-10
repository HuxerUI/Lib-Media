#include "ebml.h"

#include "codec/common.h"
#include "mp4.h"

#include <array>

namespace huxerui::media::detail {
namespace {
using namespace ebml;

constexpr std::uint32_t TickRate = Nanoseconds / DefaultTimestampScale;
constexpr std::size_t ClusterTarget = 4 * 1024 * 1024;

std::string_view CodecName(ContainerFormat format, const MediaTrack& track) {
  if (track.kind == TrackKind::Video) {
    if (format == ContainerFormat::Matroska && track.codec == CodecId::Hevc) return "V_MPEGH/ISO/HEVC";
    if (format == ContainerFormat::WebM && track.codec == CodecId::Vp9) return "V_VP9";
  }
  if (track.kind == TrackKind::Audio) {
    if (track.codec == CodecId::Opus) return "A_OPUS";
    if (format == ContainerFormat::Matroska && track.codec == CodecId::Aac) return "A_AAC";
  }
  Fail(Unsupported, "Unsupported Matroska or WebM track mapping");
}

std::int64_t Delay(const MediaTrack& track) {
  if (track.codec == CodecId::Opus) return ParseOpus(track.initialization_data, BadArgument).Delay();
  if (!track.priming_samples.value_or(0)) return 0;
  if (!track.sample_rate || *track.priming_samples > std::numeric_limits<std::int64_t>::max())
    Fail(BadArgument, "Matroska codec delay requires a valid audio sample grid");
  return ToNs({static_cast<std::int64_t>(*track.priming_samples), *track.sample_rate});
}

std::int64_t OutputTicks(std::int64_t ns) {
  return MediaTimestamp{ns, Nanoseconds}.Rescale(TickRate)->ticks;
}

void WriteTrack(Bytes& entries, ContainerFormat format, const MediaTrack& track) {
  Master(entries, 0xae, [&](Bytes& entry) {
    PutUInt(entry, 0xd7, track.id);
    PutUInt(entry, 0x73c5, track.id);
    PutUInt(entry, 0x83, track.kind == TrackKind::Video ? 1u : 2u);
    PutUInt(entry, 0x9c, 0);
    PutText(entry, 0x86, CodecName(format, track));
    if (!track.initialization_data.empty()) PutElement(entry, 0x63a2, track.initialization_data);
    if (track.kind == TrackKind::Video) {
      Master(entry, 0xe0, [&](Bytes& video) {
        PutUInt(video, 0xb0, *track.coded_width);
        PutUInt(video, 0xba, *track.coded_height);
        if (track.visible) {
          const auto rect = *track.visible;
          if (rect.x) PutUInt(video, 0x54cc, rect.x);
          if (rect.y) PutUInt(video, 0x54bb, rect.y);
          const auto right = *track.coded_width - rect.x - rect.width;
          const auto bottom = *track.coded_height - rect.y - rect.height;
          if (right) PutUInt(video, 0x54dd, right);
          if (bottom) PutUInt(video, 0x54aa, bottom);
        }
      });
    } else {
      const auto opus = track.codec == CodecId::Opus;
      const auto delay = Delay(track);
      if (delay || opus || track.priming_samples) PutUInt(entry, 0x56aa, delay);
      Master(entry, 0xe1, [&](Bytes& audio) {
        auto sampling_frequency = *track.sample_rate;
        if (opus) {
          const auto original = ParseOpus(track.initialization_data, BadArgument).input_rate;
          if (original) sampling_frequency = original;
        }
        PutFloat(audio, 0xb5, sampling_frequency);
        if (opus && sampling_frequency != 48000) PutFloat(audio, 0x78b5, 48000);
        PutUInt(audio, 0x9f, *track.channel_count);
      });
    }
    if (track.seek_preroll || track.codec == CodecId::Opus)
      PutUInt(entry, 0x56bb, track.seek_preroll ? ToNs(*track.seek_preroll) : OpusSeekPreroll);
  });
}

struct PendingBlock {
  Bytes bytes;
  std::int64_t time;
  std::int64_t order;
  std::optional<std::uint64_t> duration;
  std::optional<std::int64_t> discard;
  bool sync;
};

struct WriteTrackState {
  std::int64_t delay = 0;
  unsigned nal_length = 0;
  std::optional<std::int64_t> last_time;
  std::optional<PendingBlock> pending;
};

struct WrittenCue {
  std::uint32_t track;
  std::uint64_t time;
  std::uint64_t cluster;
  std::uint64_t block;
};

class MatroskaWriter final : public ContainerWriter {
public:
  MatroskaWriter(ContainerOutput stream, ContainerFormat format, std::span<const MediaTrack> tracks)
      : ContainerWriter(std::move(stream), tracks), webm_(format == ContainerFormat::WebM), states_(tracks.size()) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      states_[i].delay = Delay(tracks[i]);
      if (tracks[i].codec == CodecId::Hevc)
        states_[i].nal_length = HevcNalLength(tracks[i].initialization_data, false, BadArgument);
      has_video_ |= tracks[i].kind == TrackKind::Video;
    }
    Bytes header;
    Master(header, Header, [&](Bytes& ebml_header) {
      PutUInt(ebml_header, 0x4286, 1);
      PutUInt(ebml_header, 0x42f7, 1);
      PutUInt(ebml_header, 0x42f2, 4);
      PutUInt(ebml_header, 0x42f3, 8);
      PutText(ebml_header, 0x4282, webm_ ? "webm" : "matroska");
      PutUInt(ebml_header, 0x4287, 4);
      PutUInt(ebml_header, 0x4285, 2);
    });
    PutId(header, Segment);
    // An unknown-sized Segment lets sequential and random outputs use the same layout.
    Put(header, std::uint64_t{0x01ffffffffffffff});
    output.Write(header);
    segment_data_ = output.Position();
    header.clear();
    Master(header, Info, [&](Bytes& info) {
      PutUInt(info, 0x2ad7b1, DefaultTimestampScale);
      PutText(info, 0x4d80, "Lib-Media");
      PutText(info, 0x5741, "Lib-Media");
    });
    Master(header, Tracks, [&](Bytes& entries) {
      for (const auto& track : tracks) WriteTrack(entries, format, track);
    });
    output.Write(header);
  }

  bool SupportsPacketTrim() const override { return true; }

  std::optional<std::uint32_t> Write(std::uint32_t index, const ContainerPacket& packet) override {
    RequireWritable();
    auto& state = states_[index];
    if (state.pending) {
      for (std::size_t i = 0; i < states_.size(); ++i)
        if (!ended[i] && !states_[i].pending) return tracks[i].id;
      Fail(ContainerErrorCode::InvalidState, "Matroska interleaver has no pending input slot");
    }
    if (!packet.pts) Fail(ContainerErrorCode::MissingTiming, "Matroska Block requires PTS");
    const auto stored = AddTime(ToNs(*packet.pts), state.delay);
    const auto time = OutputTicks(stored);
    if (time < std::numeric_limits<std::int16_t>::min() ||
        (webm_ && (time < 0 || (state.last_time && time < *state.last_time))))
      Fail(Unsupported, "Block time is outside the supported Matroska or WebM timeline");
    const auto& track = tracks[index];
    const auto data = packet.data.AsBytes();
    const auto intrinsic = CodecDuration(track, data, BadArgument);
    const auto discard = packet.discard_padding ? std::optional{ToNs(*packet.discard_padding)} : std::nullopt;
    if (discard) {
      if (track.kind != TrackKind::Audio || !intrinsic)
        Fail(BadArgument, "Matroska discard padding requires a timed audio packet");
      auto length = ToNs(*intrinsic);
      if (packet.duration) length = std::min(length, ToNs(*packet.duration));
      if (*discard > length || *discard < -length)
        Fail(BadArgument, "Matroska discard padding exceeds its audio packet");
    }
    std::optional<std::uint64_t> duration;
    if (packet.duration && packet.duration != intrinsic) {
      // Round absolute packet endpoints so a fractional audio clock cannot accumulate duration error.
      const auto ticks = SubTime(OutputTicks(AddTime(stored, ToNs(*packet.duration))), time);
      if (ticks < 0 || (packet.duration->ticks > 0 && !ticks))
        Fail(Unsupported, "Packet duration is below the Matroska output time grid");
      // The reader applies trim to the serialized BlockDuration after the millisecond conversion.
      if (discard) {
        const auto length = ScaleTime(ticks, DefaultTimestampScale);
        if (*discard > length || *discard < -length)
          Fail(Unsupported, "Packet discard padding exceeds its duration on the Matroska output grid");
      }
      duration = static_cast<std::uint64_t>(ticks);
    }
    const bool independent = track.codec == CodecId::Hevc
        ? HevcIdr(data, state.nal_length, false, BadArgument) : track.kind == TrackKind::Audio;
    state.pending = PendingBlock{Bytes(data.begin(), data.end()), time,
                                 webm_ ? time : (packet.dts ? ToNs(*packet.dts) : stored),
                                 duration, discard, packet.sync_sample.value_or(independent)};
    state.last_time = time;
    Drain();
    return {};
  }

  void TrackEnded(std::uint32_t) override { RequireWritable(); Drain(); }

  void Finish() override {
    RequireWritable();
    std::fill(ended.begin(), ended.end(), true);
    Drain();
    Flush();
    if (!cues_.empty()) {
      std::stable_sort(cues_.begin(), cues_.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
      Bytes bytes;
      Master(bytes, Cues, [&](Bytes& entries) {
        for (std::size_t i = 0; i < cues_.size();) {
          const auto time = cues_[i].time;
          Master(entries, 0xbb, [&](Bytes& point) {
            PutUInt(point, 0xb3, time);
            do {
              const auto cue = cues_[i++];
              Master(point, 0xb7, [&](Bytes& position) {
                PutUInt(position, 0xf7, cue.track);
                PutUInt(position, 0xf1, cue.cluster);
                PutUInt(position, 0xf0, cue.block);
              });
            } while (i < cues_.size() && cues_[i].time == time);
          });
        }
      });
      output.Write(bytes);
    }
  }

private:
  void RequireWritable() const {
    if (failed_) Fail(ContainerErrorCode::InvalidState, "Matroska output failed while draining accepted packets");
  }

  void Drain() {
    try {
      for (;;) {
        std::optional<std::size_t> first;
        for (std::size_t i = 0; i < states_.size(); ++i) {
          const auto& state = states_[i];
          if (!state.pending) {
            if (!ended[i]) return;
            continue;
          }
          if (!first || state.pending->order < states_[*first].pending->order ||
              (webm_ && state.pending->order == states_[*first].pending->order &&
               tracks[i].kind == TrackKind::Audio && tracks[*first].kind == TrackKind::Video))
            first = i;
        }
        if (!first) return;
        auto& state = states_[*first];
        Emit(*first, *state.pending);
        state.pending.reset();
      }
    } catch (...) {
      failed_ = true;
      throw;
    }
  }

  void Emit(std::size_t index, const PendingBlock& packet) {
    const auto& track = tracks[index];
    const bool video_sync = track.kind == TrackKind::Video && packet.sync;
    auto relative = packet.time - cluster_time_;
    if (!cluster_.empty() &&
        (relative < std::numeric_limits<std::int16_t>::min() || relative > 2000 ||
         cluster_.size() + packet.bytes.size() > ClusterTarget || (video_sync && relative > 0))) Flush();
    if (cluster_.empty()) {
      cluster_time_ = std::max(cluster_time_, packet.time);
      relative = packet.time - cluster_time_;
      if (relative < std::numeric_limits<std::int16_t>::min())
        Fail(Unsupported, "Matroska timestamp is too far behind the current Cluster");
      PutUInt(cluster_, Timestamp, cluster_time_);
    }
    const auto position = cluster_.size();
    Bytes prefix;
    PutVint(prefix, track.id);
    Put(prefix, static_cast<std::int16_t>(relative));
    const bool simple = !packet.duration && !packet.discard;
    Put<std::uint8_t>(prefix, simple && packet.sync ? 128 : 0);
    const auto size = prefix.size() + packet.bytes.size();
    if (simple) {
      PutId(cluster_, SimpleBlock);
      PutVint(cluster_, size);
      Append(cluster_, prefix);
      Append(cluster_, packet.bytes);
    } else {
      Bytes block_header, fields;
      PutId(block_header, Block);
      PutVint(block_header, size);
      if (packet.duration) PutUInt(fields, BlockDuration, *packet.duration);
      if (packet.discard) PutInt(fields, DiscardPadding, *packet.discard);
      // A dependent frame with no known reference timestamps uses ReferenceBlock zero.
      if (!packet.sync) PutInt(fields, ReferenceBlock, 0);
      PutId(cluster_, BlockGroup);
      PutVint(cluster_, block_header.size() + size + fields.size());
      Append(cluster_, block_header);
      Append(cluster_, prefix);
      Append(cluster_, packet.bytes);
      Append(cluster_, fields);
    }
    if (packet.time >= 0 && (video_sync || (!has_video_ && !audio_cued_[index]))) {
      if (cues_.size() == MaxContainerSamples)
        Fail(ContainerErrorCode::LimitExceeded, "Matroska Cue table exceeds its limit");
      // The Cluster begins at the next output byte. The Block offset is relative to its payload.
      cues_.push_back({track.id, static_cast<std::uint64_t>(packet.time), output.Position() - segment_data_, position});
      audio_cued_[index] = true;
    }
  }

  void Flush() {
    if (cluster_.empty()) return;
    Bytes header;
    PutId(header, Cluster);
    PutVint(header, cluster_.size());
    output.Write(header);
    output.Write(cluster_);
    cluster_.clear();
    audio_cued_.fill(false);
  }

  bool webm_;
  bool has_video_ = false;
  bool failed_ = false;
  std::uint64_t segment_data_ = 0;
  std::int64_t cluster_time_ = 0;
  std::array<bool, MaxContainerTracks> audio_cued_{};
  std::vector<WriteTrackState> states_;
  Bytes cluster_;
  std::vector<WrittenCue> cues_;
};
} // namespace

void ValidateMatroska(ContainerFormat format, std::span<const MediaTrack> tracks) {
  for (const auto& track : tracks) {
    CodecName(format, track);
    if (track.padding_samples.value_or(0))
      Fail(Unsupported, "Matroska tail trim must be supplied in the last packet");
    if (track.rotation != VideoRotation::None)
      Fail(Unsupported, "Matroska output does not retain a track display rotation");
    if (track.seek_preroll) ToNs(*track.seek_preroll);
    if (track.kind == TrackKind::Video) {
      const auto width = track.coded_width.value_or(0), height = track.coded_height.value_or(0);
      if (!width || !height || track.priming_samples.value_or(0))
        Fail(BadArgument, "Invalid Matroska video dimensions or trim");
      if (track.visible && (!track.visible->width || !track.visible->height || track.visible->x >= width ||
          track.visible->y >= height || track.visible->width > width - track.visible->x ||
          track.visible->height > height - track.visible->y))
        Fail(BadArgument, "Matroska visible rectangle exceeds coded bounds");
      if (track.codec == CodecId::Hevc) {
        if (track.framing != PacketFraming::LengthPrefixedNals)
          Fail(Unsupported, "Matroska HEVC requires length-prefixed access units");
        HevcNalLength(track.initialization_data, false, BadArgument);
      } else if (track.framing != PacketFraming::CodecPacket) {
        Fail(Unsupported, "WebM VP9 requires complete codec packets");
      }
    } else {
      if (track.framing != PacketFraming::CodecPacket)
        Fail(Unsupported, "Matroska audio requires complete codec packets");
      if (track.codec == CodecId::Opus) {
        const auto opus = ParseOpus(track.initialization_data, BadArgument);
        if (track.sample_rate != 48000 || track.channel_count != opus.channels ||
            (track.priming_samples && track.priming_samples != opus.pre_skip))
          Fail(BadArgument, "Opus track does not match its 48 kHz timeline, channels, or pre-skip");
      } else {
        const auto aac = ParseAac(track.initialization_data);
        if (!aac.Succeeded()) Fail(Unsupported, aac.Error().message);
        if (track.sample_rate != aac.Value().sample_rate || track.channel_count != aac.Value().channels)
          Fail(BadArgument, "Matroska AAC track does not match AudioSpecificConfig");
      }
      Delay(track);
    }
  }
}

std::unique_ptr<ContainerWriter> OpenMatroskaWriter(ContainerOutput output, ContainerFormat format,
                                                    std::span<const MediaTrack> tracks) {
  return std::make_unique<MatroskaWriter>(std::move(output), format, tracks);
}
} // namespace huxerui::media::detail
