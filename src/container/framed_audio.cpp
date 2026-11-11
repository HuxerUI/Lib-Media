#include "framed_audio.h"

namespace huxerui::media::detail {

bool HasAudioMetadata(const MediaAudioMetadata& metadata) {
  return metadata.cover || std::ranges::any_of(AudioTags, [&](const auto& tag) { return !(metadata.*tag.field).empty(); });
}

bool IsCoverMime(std::string_view mime) {
  return mime == "image/jpeg" || mime == "image/png";
}

void ValidateAudioMetadata(const MediaAudioMetadata& metadata) {
  for (const auto& tag : AudioTags) {
    const auto& value = metadata.*tag.field;
    if (value.size() > MaxAudioText || value.find('\0') != std::string::npos)
      Fail(ContainerErrorCode::InvalidArgument, "Invalid audio text tag size or content");
  }
  if (metadata.cover && (!IsCoverMime(metadata.cover->mime_type) || metadata.cover->data.AsBytes().empty() ||
                         metadata.cover->data.AsBytes().size() > MaxContainerPacket - 64))
    Fail(ContainerErrorCode::InvalidArgument, "Invalid or oversized audio cover image");
}

void ValidateAudioPacketTime(const ContainerPacket& packet, std::int64_t start, unsigned samples, unsigned rate) {
  if ((packet.pts && *packet.pts != MediaTimestamp{start, rate}) ||
      (packet.dts && *packet.dts != MediaTimestamp{start, rate}) ||
      (packet.duration && *packet.duration != MediaTimestamp{samples, rate}))
    Fail(ContainerErrorCode::InvalidArgument, "Audio packet timing differs from its encoded sample count");
}

void FramedAudioReader::Begin(MediaTrack track, std::optional<std::uint64_t> coded_samples) {
  first_ = input.Position();
  priming_ = static_cast<std::int64_t>(track.priming_samples.value_or(0));
  padding_ = static_cast<std::int64_t>(track.padding_samples.value_or(0));
  coded_samples_ = coded_samples;
  tracks.push_back(std::move(track));
  start = MediaTimestamp{0, *tracks.front().descriptions.front().sample_rate};
  first_audible_pts[0] = start;
  if (coded_samples) {
    if (*coded_samples < std::uint64_t(priming_ + padding_) || *coded_samples > INT64_MAX)
      Fail(ContainerErrorCode::InvalidData, "Audio header has an invalid sample count");
    duration = At(static_cast<std::int64_t>(*coded_samples) - padding_);
  }
}

void FramedAudioReader::CheckEnd(std::int64_t samples) {
  if ((coded_samples_ && *coded_samples_ != std::uint64_t(samples)) || samples < priming_ + padding_)
    Fail(ContainerErrorCode::InvalidData, "Audio frame counts differ from the stream header");
  duration = At(samples - padding_);
}

std::optional<MediaPresentationRange> FramedAudioReader::PresentationRange(std::size_t) const {
  if (!duration) return {};
  return MediaPresentationRange{*start, *duration};
}

void FramedAudioReader::SeekFrame(std::uint64_t offset, std::int64_t samples) {
  input.Seek(offset);
  position_ = samples;
}

std::optional<ContainerPacket> FramedAudioReader::Read() {
  auto frame = ReadFrame(position_);
  if (!frame) {
    CheckEnd(position_);
    return {};
  }
  const auto time = At(position_);
  position_ = AddTime(position_, frame->samples);
  if (coded_samples_ && std::uint64_t(position_) > *coded_samples_)
    Fail(ContainerErrorCode::InvalidData, "Audio frames exceed the sample count");
  auto bytes = std::make_shared<const Bytes>(std::move(frame->bytes));
  return ContainerPacket{1, BufferReference(*bytes, bytes), time, time,
                         MediaTimestamp{frame->samples, time.timescale}, frame->restartable};
}

void FramedAudioReader::BuildIndex() {
  if (!index_.empty()) return;
  const auto previous = input.Position();
  std::vector<Entry> entries;
  std::int64_t samples = 0;
  try {
    input.Seek(first_);
    for (;;) {
      const auto offset = input.Position();
      auto frame = ReadFrame(samples);
      if (!frame) break;
      if (entries.size() == MaxContainerSamples)
        Fail(ContainerErrorCode::LimitExceeded, "Audio frame index exceeds its entry limit");
      entries.push_back({offset, samples, frame->restartable});
      samples = AddTime(samples, frame->samples);
    }
    CheckEnd(samples);
  } catch (...) {
    input.Seek(previous);
    throw;
  }
  input.Seek(previous);
  index_ = std::move(entries);
}

std::optional<SeekPoint> FramedAudioReader::Find(std::uint32_t id, MediaTimestamp target,
                                                 SeekDirection direction, bool seek) {
  if (!input.Random()) return ContainerReader::Find(id, target, direction, seek);
  BuildIndex();
  auto point = std::lower_bound(index_.begin(), index_.end(), target, [&](const Entry& entry, MediaTimestamp time) {
    return At(entry.samples).Compare(time) < 0;
  });
  if (direction == SeekDirection::AtOrBefore && (point == index_.end() || At(point->samples) != target)) {
    if (point == index_.begin()) return {};
    --point;
  }
  if (point == index_.end()) return {};
  auto decode = point;
  if (format == ContainerFormat::Mp3) {
    for (unsigned i = 0; i < 2 && decode != index_.begin(); ++i) --decode;
    while (decode != index_.begin() && !decode->restartable) --decode;
    if (!decode->restartable) Fail(ContainerErrorCode::IndexUnavailable, "No MPEG audio reservoir restart point");
  }
  if (seek) SeekFrame(decode->offset, decode->samples);
  const auto kind = format == ContainerFormat::Mp3 ? SeekPoint::Kind::NeedsPreroll : SeekPoint::Kind::Independent;
  return SeekPoint{id, At(point->samples), kind, point->offset,
                   At(decode->samples)};
}

} // namespace huxerui::media::detail
