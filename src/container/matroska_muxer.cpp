#include "ebml.h"

#include "codec/common.h"
#include "mp4.h"

#include <array>

namespace huxerui::media::detail {
namespace {
using namespace ebml;

constexpr std::size_t ClusterTarget = 4 * 1024 * 1024;

std::string_view CodecName(ContainerFormat format, const MediaTrack& track) {
  const auto& description = track.descriptions.front();
  if (track.kind == TrackKind::Video) {
    if (format == ContainerFormat::Matroska && description.codec == CodecId::H264) return "V_MPEG4/ISO/AVC";
    if (format == ContainerFormat::Matroska && description.codec == CodecId::Hevc) return "V_MPEGH/ISO/HEVC";
    if (description.codec == CodecId::Vp8) return "V_VP8";
    if (description.codec == CodecId::Vp9) return "V_VP9";
  }
  if (track.kind == TrackKind::Audio) {
    if (description.codec == CodecId::Opus) return "A_OPUS";
    if (format == ContainerFormat::Matroska && description.codec == CodecId::Aac) return "A_AAC";
  }
  Fail(Unsupported, "Unsupported Matroska or WebM track mapping");
}

std::int64_t Delay(const MediaTrack& track) {
  const auto& description = track.descriptions.front();
  if (description.codec == CodecId::Opus) return ParseOpus(description.initialization_data, BadArgument).Delay();
  if (!track.priming_samples.value_or(0)) return 0;
  if (!description.sample_rate || *track.priming_samples > std::numeric_limits<std::int64_t>::max())
    Fail(BadArgument, "Matroska codec delay requires a valid audio sample grid");
  return ToNs({static_cast<std::int64_t>(*track.priming_samples), *description.sample_rate});
}

void WriteTrack(Bytes& entries, ContainerFormat format, const MediaTrack& track) {
  const auto& description = track.descriptions.front();
  Master(entries, 0xae, [&](Bytes& entry) {
    PutUInt(entry, 0xd7, track.id);
    PutUInt(entry, 0x73c5, track.id);
    PutUInt(entry, 0x83, track.kind == TrackKind::Video ? 1u : 2u);
    PutUInt(entry, 0x9c, 0);
    PutText(entry, 0x86, CodecName(format, track));
    if (!description.initialization_data.empty()) PutElement(entry, 0x63a2, description.initialization_data);
    if (track.default_duration) PutUInt(entry, 0x23e383, ToNs(*track.default_duration));
    if (track.kind == TrackKind::Video) {
      Master(entry, 0xe0, [&](Bytes& video) {
        PutUInt(video, 0xb0, *description.coded_width);
        PutUInt(video, 0xba, *description.coded_height);
        if (description.visible) {
          const auto rect = *description.visible;
          if (rect.x) PutUInt(video, 0x54cc, rect.x);
          if (rect.y) PutUInt(video, 0x54bb, rect.y);
          const auto right = *description.coded_width - rect.x - rect.width;
          const auto bottom = *description.coded_height - rect.y - rect.height;
          if (right) PutUInt(video, 0x54dd, right);
          if (bottom) PutUInt(video, 0x54aa, bottom);
        }
      });
    } else {
      const auto opus = description.codec == CodecId::Opus;
      const auto delay = Delay(track);
      if (delay || opus || track.priming_samples) PutUInt(entry, 0x56aa, delay);
      Master(entry, 0xe1, [&](Bytes& audio) {
        auto sampling_frequency = *description.sample_rate;
        if (opus) {
          const auto original = ParseOpus(description.initialization_data, BadArgument).input_rate;
          if (original) sampling_frequency = original;
        }
        PutFloat(audio, 0xb5, sampling_frequency);
        if (opus && sampling_frequency != 48000) PutFloat(audio, 0x78b5, 48000);
        PutUInt(audio, 0x9f, *description.channel_count);
      });
    }
    if (track.seek_preroll || description.codec == CodecId::Opus)
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
  MatroskaWriter(ContainerOutput stream, ContainerFormat format, std::span<const MediaTrack> tracks, std::int64_t scale)
      : ContainerWriter(std::move(stream), tracks), webm_(format == ContainerFormat::WebM),
        scale_(scale), states_(tracks.size()) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      states_[i].delay = Delay(tracks[i]);
      const auto& description = tracks[i].descriptions.front();
      if (description.codec == CodecId::Hevc)
        states_[i].nal_length = HevcNalLength(description.initialization_data, false, BadArgument);
      if (description.codec == CodecId::H264)
        states_[i].nal_length = ParseAvc(description.initialization_data).Value().length_size;
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
      PutUInt(info, 0x2ad7b1, scale_);
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
    const auto& description = track.descriptions.front();
    const auto intrinsic = CodecDuration(description, data, BadArgument);
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
    const auto implied_duration = intrinsic ? intrinsic : track.default_duration;
    if (packet.duration && packet.duration != implied_duration) {
      // Round absolute packet endpoints so a fractional audio clock cannot accumulate duration error.
      const auto ticks = SubTime(OutputTicks(AddTime(stored, ToNs(*packet.duration))), time);
      if (ticks < 0 || (packet.duration->ticks > 0 && !ticks))
        Fail(Unsupported, "Packet duration is below the Matroska output time grid");
      // The reader applies trim to the serialized BlockDuration on the output grid.
      if (discard) {
        const auto length = ScaleTime(ticks, scale_);
        if (*discard > length || *discard < -length)
          Fail(Unsupported, "Packet discard padding exceeds its duration on the Matroska output grid");
      }
      duration = static_cast<std::uint64_t>(ticks);
    }
    bool independent = track.kind == TrackKind::Audio;
    // AVC and HEVC can derive a sync flag from an IDR. VPx uses the caller's container sync flag.
    if (description.codec == CodecId::H264) independent = AvcIdr(data, state.nal_length, BadArgument);
    if (description.codec == CodecId::Hevc) independent = HevcIdr(data, state.nal_length, false, BadArgument);
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
  std::int64_t OutputTicks(std::int64_t ns) const {
    auto ticks = ns / scale_;
    const auto remainder = ns % scale_;
    const auto half = scale_ / 2 + scale_ % 2;
    if (remainder >= half) ++ticks;
    if (remainder <= -half) --ticks;
    ScaleTime(ticks, scale_);
    return ticks;
  }

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
    auto relative = SubTime(packet.time, cluster_time_);
    const auto maximum = std::min<std::int64_t>(2000000000 / scale_, std::numeric_limits<std::int16_t>::max());
    if (!cluster_.empty() &&
        (relative < std::numeric_limits<std::int16_t>::min() || relative > maximum ||
         cluster_.size() + packet.bytes.size() > ClusterTarget || (video_sync && relative > 0))) Flush();
    if (cluster_.empty()) {
      cluster_time_ = std::max<std::int64_t>(0, packet.time);
      relative = SubTime(packet.time, cluster_time_);
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
  std::int64_t scale_;
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
    const auto& description = track.descriptions.front();
    if (track.default_duration) {
      const auto ns = track.default_duration->Rescale(Nanoseconds);
      if (!ns || ns->ticks <= 0 || *ns != *track.default_duration)
        Fail(BadArgument, "Matroska default duration must be a positive whole number of nanoseconds");
    }
    if (track.padding_samples.value_or(0))
      Fail(Unsupported, "Matroska tail trim must be supplied in the last packet");
    if (track.rotation != VideoRotation::None)
      Fail(Unsupported, "Matroska output does not retain a track display rotation");
    if (track.seek_preroll) ToNs(*track.seek_preroll);
    if (track.kind == TrackKind::Video) {
      const auto width = description.coded_width.value_or(0), height = description.coded_height.value_or(0);
      if (!width || !height || track.priming_samples.value_or(0))
        Fail(BadArgument, "Invalid Matroska video dimensions or trim");
      if (description.visible && (!description.visible->width || !description.visible->height || description.visible->x >= width ||
          description.visible->y >= height || description.visible->width > width - description.visible->x ||
          description.visible->height > height - description.visible->y))
        Fail(BadArgument, "Matroska visible rectangle exceeds coded bounds");
      if (description.codec == CodecId::H264 || description.codec == CodecId::Hevc) {
        if (description.framing != PacketFraming::LengthPrefixedNals)
          Fail(Unsupported, "Matroska AVC and HEVC require length-prefixed access units");
        if (description.codec == CodecId::H264) {
          const auto config = ParseAvc(description.initialization_data);
          if (!config.Succeeded()) Fail(Unsupported, config.Error().message);
        } else {
          HevcNalLength(description.initialization_data, false, BadArgument);
        }
      } else if (description.framing != PacketFraming::CodecPacket) {
        Fail(Unsupported, "Matroska VP8 and VP9 require complete codec packets");
      } else if (description.codec == CodecId::Vp8 && !description.initialization_data.empty()) {
        Fail(BadArgument, "VP8 has no Matroska codec private data");
      }
    } else {
      if (description.framing != PacketFraming::CodecPacket)
        Fail(Unsupported, "Matroska audio requires complete codec packets");
      if (description.codec == CodecId::Opus) {
        const auto opus = ParseOpus(description.initialization_data, BadArgument);
        if (description.sample_rate != 48000 || description.channel_count != opus.channels ||
            (track.priming_samples && track.priming_samples != opus.pre_skip))
          Fail(BadArgument, "Opus track does not match its 48 kHz timeline, channels, or pre-skip");
      } else {
        const auto aac = ParseAac(description.initialization_data);
        if (!aac.Succeeded()) Fail(Unsupported, aac.Error().message);
        if (description.sample_rate != aac.Value().sample_rate || description.channel_count != aac.Value().channels)
          Fail(BadArgument, "Matroska AAC track does not match AudioSpecificConfig");
      }
      Delay(track);
    }
  }
}

std::unique_ptr<ContainerWriter> OpenMatroskaWriter(ContainerOutput output, ContainerFormat format,
                                                    std::span<const MediaTrack> tracks, const MediaMuxerOptions& options) {
  const auto scale = options.ebml_timestamp_scale.value_or(std::chrono::nanoseconds{DefaultTimestampScale});
  return std::make_unique<MatroskaWriter>(std::move(output), format, tracks, scale.count());
}
} // namespace huxerui::media::detail
