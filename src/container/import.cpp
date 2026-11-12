#include "import.h"

namespace huxerui::media::detail {

Bytes ReadImportBytes(ContainerInput& input, std::size_t limit) {
  if (input.Size().value_or(0) > limit) Fail(ContainerErrorCode::LimitExceeded, "Import exceeds its byte limit");
  Bytes data;
  if (const auto size = input.Size()) data.reserve(static_cast<std::size_t>(*size));
  while (const auto block = input.Peek(65536).size()) {
    if (block > limit - data.size()) Fail(ContainerErrorCode::LimitExceeded, "Import exceeds its byte limit");
    Append(data, input.ReadBytes(block));
  }
  return data;
}

void IndexedImport::Add(ContainerPacket packet, std::optional<std::uint64_t> offset) {
  if (entries_.size() >= MaxContainerSamples) Fail(ContainerErrorCode::LimitExceeded, "Import packet limit exceeded");
  entries_.push_back({std::move(packet), offset});
}

void IndexedImport::Complete() {
  if (entries_.empty()) Fail(ContainerErrorCode::InvalidData, "Import contains no supported packets");
  std::optional<MediaTimestamp> end;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    const auto& packet = entries_[i].packet;
    if (!packet.pts) continue;
    const auto track = PacketTrack(tracks, packet.track_id);
    if (input.Random() && (tracks[track].kind != TrackKind::Video || packet.sync_sample == true))
      anchors_[track].push_back(i);
    if (!start || packet.pts->Compare(*start) < 0) start = *packet.pts;
    if (!packet.duration) continue;
    const auto scale = CommonScale(packet.pts->timescale, packet.duration->timescale);
    const MediaTimestamp last{AddTime(ExactTicks(*packet.pts, scale), ExactTicks(*packet.duration, scale)), scale};
    auto& range = ranges_[track];
    if (!range) range = MediaPresentationRange{*packet.pts, last};
    if (packet.pts->Compare(range->start) < 0) range->start = *packet.pts;
    if (last.Compare(range->end) > 0) range->end = last;
    if (!end || last.Compare(*end) > 0) end = last;
  }
  for (auto& track_anchors : anchors_)
    std::ranges::sort(track_anchors, [&](std::size_t a, std::size_t b) {
      const auto order = entries_[a].packet.pts->Compare(*entries_[b].packet.pts);
      return order == 0 ? a < b : order < 0;
    });
  duration = end;
}

std::optional<ContainerPacket> IndexedImport::Read() {
  while (cursor_ < entries_.size()) {
    const auto index = cursor_++;
    const auto& packet = entries_[index].packet;
    if (index >= seek_floors_[PacketTrack(tracks, packet.track_id)]) return packet;
  }
  return {};
}

std::optional<SeekPoint> IndexedImport::Find(std::uint32_t id, MediaTimestamp time, SeekDirection direction, bool seek) {
  if (!input.Random()) return ContainerReader::Find(id, time, direction, seek);
  if (!start) Fail(ContainerErrorCode::IndexUnavailable, "Import has no presentation timestamps");
  const auto reference = PacketTrack(tracks, id);
  const auto locate = [&](std::size_t track, MediaTimestamp target, SeekDirection wanted) -> std::optional<std::size_t> {
    const auto& points = anchors_[track];
    const auto before = [&](std::size_t index, MediaTimestamp value) {
      return entries_[index].packet.pts->Compare(value) < 0;
    };
    auto at = std::lower_bound(points.begin(), points.end(), target, before);
    if (wanted == SeekDirection::AtOrBefore && (at == points.end() || entries_[*at].packet.pts != target)) {
      if (at == points.begin()) return {};
      const auto previous = *std::prev(at);
      at = std::lower_bound(points.begin(), at, *entries_[previous].packet.pts, before);
    }
    if (at == points.end()) return {};
    return *at;
  };
  const auto chosen = locate(reference, time, direction);
  if (!chosen) return {};
  const auto& entry = entries_[*chosen];
  if (seek) {
    cursor_ = *chosen;
    seek_floors_.fill(0);
    seek_floors_[reference] = *chosen;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (i == reference || (!selected.empty() && std::ranges::find(selected, tracks[i].id) == selected.end())) continue;
      const auto floor = locate(i, *entry.packet.pts, SeekDirection::AtOrBefore);
      seek_floors_[i] = floor ? *floor : locate(i, *entry.packet.pts, SeekDirection::AtOrAfter).value_or(entries_.size());
      cursor_ = std::min(cursor_, seek_floors_[i]);
    }
  }
  return SeekPoint{id, *entry.packet.pts, SeekPoint::Kind::Independent, entry.offset,
      *entry.packet.pts};
}

} // namespace huxerui::media::detail
