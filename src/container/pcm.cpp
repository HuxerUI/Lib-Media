#include "pcm.h"

#include "codec/common.h"
#include "framed_audio.h"
#include "format/packed_pcm.h"

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

void ValidatePcmTrack(const MediaTrack& track) {
  const auto& description = track.descriptions.front();
  const auto channels = description.channel_count.value_or(0);
  if (!description.sample_rate.value_or(0) || !channels)
    Fail(ContainerErrorCode::InvalidArgument, "PCM requires a positive sample rate and channel count");
  if (channels > 2 || !DescribePackedPcm(description.codec).bits || !description.initialization_data.empty() ||
      !track.edits.empty() || track.priming_samples.value_or(0) || track.padding_samples.value_or(0))
    Fail(ContainerErrorCode::Unsupported, "Output requires packed mono or stereo PCM on a contiguous sample clock");
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
