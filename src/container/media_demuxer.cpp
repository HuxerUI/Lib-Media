#include "container_io.h"

namespace huxerui::media {
namespace {
using namespace detail;

std::unique_ptr<ContainerReader> OpenReader(ContainerInput input, std::optional<ContainerFormat> hint) {
  const auto prefix = input.Peek(12);
  if (prefix.size() >= 4 && Number<std::uint32_t>(prefix) == 0x1a45dfa3u) {
    if (hint && *hint != ContainerFormat::Matroska && *hint != ContainerFormat::WebM)
      Fail(ContainerErrorCode::UnrecognizedFormat, "EBML input does not match the format hint");
    return OpenMatroskaReader(std::move(input), hint);
  }
  std::optional<ContainerFormat> recognized;
  if (prefix.size() >= 4 && Number<std::uint32_t>(prefix) == Tag("OggS")) recognized = ContainerFormat::Ogg;
  if (prefix.size() >= 4 && Number<std::uint32_t>(prefix) == Tag("fLaC")) recognized = ContainerFormat::Flac;
  if (prefix.size() >= 12 && Number<std::uint32_t>(prefix, 8) == Tag("WAVE")) {
    const auto type = Number<std::uint32_t>(prefix);
    if (type == Tag("RIFF")) recognized = ContainerFormat::Wave;
    if (type == Tag("RF64")) recognized = ContainerFormat::Rf64;
  }
  if (!recognized && prefix.size() >= 8) {
    const auto box = Number<std::uint32_t>(prefix, 4);
    if (box == Tag("ftyp") || box == Tag("moov") || box == Tag("mdat") || box == Tag("free") || box == Tag("wide"))
      recognized = ContainerFormat::Mp4;
  }
  if (!recognized && prefix.size() >= 2 && (Number<std::uint16_t>(prefix) & 0xfff6) == 0xfff0)
    recognized = ContainerFormat::Adts;
  if (!recognized && LooksLikeMp3(prefix)) recognized = ContainerFormat::Mp3;

  if (!recognized || (hint && *recognized != *hint &&
      !(*recognized == ContainerFormat::Mp4 && *hint == ContainerFormat::Mov)))
    Fail(ContainerErrorCode::UnrecognizedFormat, "Input bytes do not identify the requested or a supported container");
  switch (*recognized) {
    case ContainerFormat::Wave:
    case ContainerFormat::Rf64: return OpenWaveReader(std::move(input), *recognized);
    case ContainerFormat::Adts: return OpenAdtsReader(std::move(input));
    case ContainerFormat::Ogg: return OpenOggReader(std::move(input));
    case ContainerFormat::Flac: return OpenFlacReader(std::move(input));
    case ContainerFormat::Mp3: return OpenMp3Reader(std::move(input));
    case ContainerFormat::Mp4:
    case ContainerFormat::Mov: return OpenMp4Reader(std::move(input), hint);
    case ContainerFormat::Matroska:
    case ContainerFormat::WebM: break;
  }
  Fail(ContainerErrorCode::Unsupported, "Unsupported input container");
}

void ValidateSeek(ContainerReader& reader, std::uint32_t id, MediaTimestamp time, SeekDirection direction) {
  PacketTrack(reader.tracks, id);
  if (!time.timescale || (direction != SeekDirection::AtOrBefore && direction != SeekDirection::AtOrAfter))
    Fail(ContainerErrorCode::InvalidArgument, "Invalid seek time or direction");
}

bool Selected(const ContainerReader& reader, std::uint32_t id) {
  return reader.selected.empty() || std::ranges::find(reader.selected, id) != reader.selected.end();
}
} // namespace

MediaDemuxer::MediaDemuxer(std::unique_ptr<ContainerReader> reader) : reader_(std::move(reader)) {}
MediaDemuxer::MediaDemuxer(MediaDemuxer&&) noexcept = default;
MediaDemuxer& MediaDemuxer::operator=(MediaDemuxer&&) noexcept = default;
MediaDemuxer::~MediaDemuxer() = default;

ContainerResult<MediaDemuxer> MediaDemuxer::Open(InputStream input, std::optional<ContainerFormat> hint) {
  return ContainerCall<MediaDemuxer>([&] { return MediaDemuxer(OpenReader(ContainerInput(std::move(input)), hint)); });
}

ContainerResult<MediaDemuxer> MediaDemuxer::Open(RandomAccessStream input, std::optional<ContainerFormat> hint) {
  return ContainerCall<MediaDemuxer>([&] { return MediaDemuxer(OpenReader(ContainerInput(std::move(input)), hint)); });
}

ContainerReader& MediaDemuxer::Require() const {
  if (!reader_ || reader_->failed) throw std::logic_error("Container input session is unavailable");
  return *reader_;
}

ContainerFormat MediaDemuxer::Format() const { return Require().format; }
ContainerLayout MediaDemuxer::Layout() const { return Require().layout; }
std::span<const MediaTrack> MediaDemuxer::Tracks() const { return Require().tracks; }
std::optional<MediaTimestamp> MediaDemuxer::StartTime() const { return Require().start; }
std::optional<MediaTimestamp> MediaDemuxer::Duration() const { return Require().duration; }

std::optional<MediaPresentationRange> MediaDemuxer::PresentationRange(std::uint32_t track_id) const {
  const auto& reader = Require();
  const auto track = std::ranges::find(reader.tracks, track_id, &MediaTrack::id);
  if (track == reader.tracks.end()) return {};
  return reader.PresentationRange(static_cast<std::size_t>(track - reader.tracks.begin()));
}

ContainerResult<void> MediaDemuxer::SelectTracks(std::span<const std::uint32_t> tracks) {
  return ContainerCall<void>([&] {
    auto& reader = Require();
    if (tracks.size() > reader.tracks.size()) Fail(ContainerErrorCode::InvalidArgument, "Too many selected tracks");
    std::vector<std::uint32_t> selection;
    for (auto id : tracks) {
      PacketTrack(reader.tracks, id);
      if (std::ranges::find(selection, id) != selection.end())
        Fail(ContainerErrorCode::InvalidArgument, "Duplicate selected track");
      selection.push_back(id);
    }
    auto descriptions = reader.describe;
    for (auto& track : reader.tracks) {
      const auto selected = selection.empty() || std::ranges::find(selection, track.id) != selection.end();
      if (selected && !Selected(reader, track.id) && std::ranges::find(descriptions, track.id) == descriptions.end())
        descriptions.push_back(track.id);
    }
    reader.selected = std::move(selection);
    reader.describe = std::move(descriptions);
  });
}

ContainerResult<std::optional<DemuxItem>> MediaDemuxer::Read() {
  auto result = ContainerCall<std::optional<DemuxItem>>([&]() -> std::optional<DemuxItem> {
    auto& reader = Require();
    for (;;) {
      std::optional<DemuxItem> item;
      if (reader.pending) {
        item = DemuxItem(std::move(*reader.pending));
        reader.pending.reset();
      } else {
        item = reader.Read();
      }
      if (!item) return {};
      if (auto* track = std::get_if<MediaTrack>(&*item)) {
        if (!Selected(reader, track->id)) continue;
        std::erase(reader.describe, track->id);
        return item;
      }
      auto& packet = std::get<ContainerPacket>(*item);
      if (!Selected(reader, packet.track_id)) continue;
      if (std::erase(reader.describe, packet.track_id)) {
        const auto index = PacketTrack(reader.tracks, packet.track_id);
        // The packet retains its bytes while its effective description is delivered first.
        reader.pending = std::move(packet);
        return DemuxItem(reader.tracks[index]);
      }
      return item;
    }
  });
  if (!result.Succeeded() && reader_) reader_->failed = true;
  return result;
}

ContainerResult<std::optional<SeekPoint>> MediaDemuxer::FindSeekPoint(
    std::uint32_t track_id, MediaTimestamp time, SeekDirection direction) {
  return ContainerCall<std::optional<SeekPoint>>([&] {
    auto& reader = Require();
    ValidateSeek(reader, track_id, time, direction);
    return reader.Find(track_id, time, direction, false);
  });
}

ContainerResult<std::optional<SeekPoint>> MediaDemuxer::Seek(
    std::uint32_t track_id, MediaTimestamp time, SeekDirection direction) {
  return ContainerCall<std::optional<SeekPoint>>([&] {
    auto& reader = Require();
    ValidateSeek(reader, track_id, time, direction);
    std::vector<std::uint32_t> descriptions;
    for (auto& track : reader.tracks) {
      if (Selected(reader, track.id)) descriptions.push_back(track.id);
    }
    auto point = reader.Find(track_id, time, direction, true);
    if (point) {
      reader.pending.reset();
      reader.describe = std::move(descriptions);
    }
    return point;
  });
}

ContainerResult<void> MediaDemuxer::Close() {
  return ContainerCall<void>([&] {
    auto reader = std::move(reader_);
    if (reader) reader->input.Close();
  });
}
} // namespace huxerui::media
