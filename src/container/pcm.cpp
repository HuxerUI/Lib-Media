#include "pcm.h"

#include "codec/common.h"
#include "framed_audio.h"
#include "format/packed_pcm.h"

#include <bit>

namespace huxerui::media::detail {

CodecId SignedPcmCodec(unsigned bits, bool big_endian, bool floating_point) {
  if (floating_point) {
    if (bits == 32) return big_endian ? CodecId::PcmF32Be : CodecId::PcmF32Le;
  } else {
    switch (bits) {
      case 16: return big_endian ? CodecId::PcmS16Be : CodecId::PcmS16Le;
      case 24: return big_endian ? CodecId::PcmS24Be : CodecId::PcmS24Le;
      case 32: return big_endian ? CodecId::PcmS32Be : CodecId::PcmS32Le;
    }
  }
  Fail(ContainerErrorCode::Unsupported, "Unsupported PCM sample format");
}

MediaTrack PcmTrack(CodecId codec, std::uint32_t rate, unsigned channels) {
  return {.id = 1, .kind = TrackKind::Audio, .timescale = rate,
      .descriptions = {{.codec = codec, .sample_rate = rate, .channel_count = channels}}};
}

namespace {
constexpr std::uint32_t ChannelBitmap = 0x10000;

std::optional<std::uint32_t> PcmSpeakerMask(ByteView bytes, unsigned channels, ContainerErrorCode error) {
  if (bytes.size() < 12) Fail(error, "Truncated PCM channel layout");
  const auto tag = Number<std::uint32_t>(bytes);
  const auto mask = Number<std::uint32_t>(bytes, 4);
  const auto count = Number<std::uint32_t>(bytes, 8);
  if (count > (bytes.size() - 12) / 20 || (!tag && count != channels) || (tag && count))
    Fail(error, "Invalid PCM channel description count");
  if (tag == ChannelBitmap) {
    if (mask && std::popcount(mask) != static_cast<int>(channels))
      Fail(error, "PCM channel bitmap does not match the channel count");
    return mask;
  }
  if (tag && (tag & 0xffff) != channels)
    Fail(error, "PCM channel layout tag does not match the channel count");
  switch (tag) {
    case (100u << 16) | 1: return 0x4u;
    case (101u << 16) | 2: return 0x3u;
    case (121u << 16) | 6: return 0x60fu;
  }
  return {};
}
} // namespace

void ReadPcmChannelLayout(MediaSampleDescription& description, Bytes layout) {
  description.speaker_mask = PcmSpeakerMask(layout, *description.channel_count, ContainerErrorCode::InvalidData);
  if (!description.speaker_mask) description.initialization_data = std::move(layout);
}

Bytes WritePcmChannelLayout(const MediaSampleDescription& description) {
  if (!description.speaker_mask) return description.initialization_data;
  Bytes layout;
  Put(layout, ChannelBitmap);
  Put(layout, *description.speaker_mask);
  Put(layout, 0u);
  return layout;
}

void ValidatePcmChannelLayout(const MediaSampleDescription& description) {
  const auto channels = description.channel_count.value_or(0);
  constexpr auto invalid = ContainerErrorCode::InvalidArgument;
  if (const auto mask = description.speaker_mask; mask && *mask && std::popcount(*mask) != static_cast<int>(channels))
    Fail(invalid, "PCM speaker mask does not match the channel count");
  if (!description.initialization_data.empty()) {
    if (description.speaker_mask || description.initialization_data.size() > 4096)
      Fail(invalid, "Conflicting or oversized PCM channel layout");
    PcmSpeakerMask(description.initialization_data, channels, invalid);
  }
}

void ValidatePcmTrack(const MediaTrack& track) {
  const auto& description = track.descriptions.front();
  const auto channels = description.channel_count.value_or(0);
  if (!description.sample_rate.value_or(0) || !channels)
    Fail(ContainerErrorCode::InvalidArgument, "PCM requires a positive sample rate and channel count");
  if (channels > 8 || !DescribePackedPcm(description.codec).bits ||
      !track.edits.empty() || track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(ContainerErrorCode::Unsupported, "Output requires packed PCM with up to eight channels on a contiguous sample clock");
  ValidatePcmChannelLayout(description);
}

unsigned PcmPacketFrames(const ContainerPacket& packet, unsigned alignment, std::int64_t start, unsigned rate) {
  const auto size = packet.data.AsBytes().size();
  if (size % alignment || packet.sync_sample == false)
    Fail(ContainerErrorCode::InvalidArgument, "PCM packet requires complete independent sample frames");
  const auto frames = static_cast<unsigned>(size / alignment);
  ValidateAudioPacketTime(packet, start, frames, rate);
  return frames;
}

PcmReader::PcmReader(ContainerInput input, ContainerFormat format, MediaTrack track, std::uint64_t bytes, unsigned alignment)
    : PcmReader(std::move(input), format) {
  Begin(std::move(track), bytes, alignment);
}

void PcmReader::Begin(MediaTrack track, std::uint64_t bytes, unsigned alignment, unsigned padding) {
  if (!alignment || bytes % alignment || bytes / alignment > INT64_MAX)
    Fail(ContainerErrorCode::InvalidData, "PCM data ends inside a sample frame or exceeds its sample clock");
  alignment_ = alignment;
  rate_ = *track.descriptions.front().sample_rate;
  offset_ = input.Position();
  frames_ = static_cast<std::int64_t>(bytes / alignment);
  padding_ = padding;
  start = MediaTimestamp{0, rate_};
  duration = MediaTimestamp{frames_, rate_};
  tracks.push_back(std::move(track));
}

std::optional<ContainerPacket> PcmReader::Read() {
  if (cursor_ == frames_) {
    if (padding_) {
      input.Skip(padding_);
      padding_ = 0;
    }
    return {};
  }
  // Emit bounded blocks without ever splitting an interleaved sample frame.
  const auto frames = std::min<std::int64_t>(frames_ - cursor_, std::max(1u, 16384 / alignment_));
  auto bytes = Own(input.ReadBytes(static_cast<std::size_t>(frames) * alignment_));
  const MediaTimestamp time{cursor_, rate_};
  cursor_ += frames;
  return ContainerPacket{tracks.front().id, std::move(bytes), time, time, MediaTimestamp{frames, rate_}, true};
}

std::optional<SeekPoint> PcmReader::Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) {
  if (seek && !input.Random()) Fail(ContainerErrorCode::NeedRandomAccess, "PCM input cannot seek");
  if (!frames_) return {};
  const MediaTimestamp first{0, rate_}, last{frames_ - 1, rate_};
  std::int64_t frame = 0;
  if (time.Compare(first) < 0) {
    if (direction == SeekDirection::AtOrBefore) return {};
  } else if (time.Compare(last) > 0) {
    if (direction == SeekDirection::AtOrAfter) return {};
    frame = last.ticks;
  } else {
    frame = time.Rescale(rate_)->ticks;
    const auto comparison = MediaTimestamp{frame, rate_}.Compare(time);
    if (direction == SeekDirection::AtOrBefore && comparison > 0) --frame;
    if (direction == SeekDirection::AtOrAfter && comparison < 0) ++frame;
  }
  const auto offset = AddOffset(offset_, static_cast<std::uint64_t>(frame) * alignment_);
  if (seek) {
    input.Seek(offset);
    cursor_ = frame;
  }
  return SeekPoint{id, {frame, rate_}, SeekPoint::Kind::Independent, offset, {frame, rate_}};
}

std::optional<MediaPresentationRange> PcmReader::PresentationRange(std::size_t) const {
  return MediaPresentationRange{*start, *duration};
}

} // namespace huxerui::media::detail
