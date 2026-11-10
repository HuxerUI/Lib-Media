#include "container_io.h"

namespace huxerui::media {
namespace {
using namespace detail;
using Requirement = MediaMuxer::OutputRequirement;

Requirement Validate(ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout) {
  if (tracks.empty()) Fail(ContainerErrorCode::InvalidArgument, "A container needs at least one track");
  if (tracks.size() > MaxContainerTracks) Fail(ContainerErrorCode::LimitExceeded, "Too many output tracks");
  for (auto it = tracks.begin(); it != tracks.end(); ++it) {
    if (!it->id || std::ranges::find(tracks.begin(), it, it->id, &MediaTrack::id) != it)
      Fail(ContainerErrorCode::InvalidArgument, "Output track IDs must be nonzero and unique");
    if (it->timescale == 0) Fail(ContainerErrorCode::InvalidArgument, "Zero output track timescale");
    if (it->seek_preroll && (!it->seek_preroll->timescale || it->seek_preroll->ticks < 0))
      Fail(ContainerErrorCode::InvalidArgument, "Invalid track seek preroll");
    if (it->seek_preroll && it->seek_preroll->ticks &&
        format != ContainerFormat::Matroska && format != ContainerFormat::WebM && format != ContainerFormat::Ogg)
      Fail(ContainerErrorCode::Unsupported, "Output format cannot retain track seek preroll");
    if (it->initialization_data.size() > 65536)
      Fail(ContainerErrorCode::LimitExceeded, "Output track configuration exceeds 64 KiB");
    if (it->presentation_edit && it->presentation_edit->start.Compare(it->presentation_edit->end) != -1)
      Fail(ContainerErrorCode::InvalidArgument, "Invalid track presentation edit");
    if ((it->presentation_edit || it->pixel_aspect_ratio || !it->video_extensions.empty()) &&
        format != ContainerFormat::Mp4 && format != ContainerFormat::Mov)
      Fail(ContainerErrorCode::Unsupported, "Output format cannot retain the supplied presentation metadata");
    if (it->kind != TrackKind::Video && (it->pixel_aspect_ratio || !it->video_extensions.empty()))
      Fail(ContainerErrorCode::InvalidArgument, "Visual metadata requires a video track");
    if (it->audio_metadata && format != ContainerFormat::Flac && format != ContainerFormat::Mp3)
      Fail(ContainerErrorCode::Unsupported, "Output format cannot retain the supplied audio tags");
  }
  if (layout != ContainerLayout::Default && (format != ContainerFormat::Mp4 ||
      (layout != ContainerLayout::Fragmented && layout != ContainerLayout::FastStart)))
    Fail(ContainerErrorCode::Unsupported, "Unsupported output layout");
  switch (format) {
    case ContainerFormat::Wave:
    case ContainerFormat::Rf64:
      ValidateWave(tracks);
      return Requirement::RandomWrite;
    case ContainerFormat::Adts:
      ValidateAdts(tracks);
      return Requirement::SequentialWrite;
    case ContainerFormat::Ogg:
      ValidateOgg(tracks);
      return Requirement::SequentialWrite;
    case ContainerFormat::Mp4:
    case ContainerFormat::Mov:
      ValidateMp4(format, tracks, layout);
      if (layout == ContainerLayout::FastStart) return Requirement::RandomReadWrite;
      return layout == ContainerLayout::Fragmented ? Requirement::SequentialWrite : Requirement::RandomWrite;
    case ContainerFormat::Matroska:
    case ContainerFormat::WebM:
      ValidateMatroska(format, tracks);
      return Requirement::SequentialWrite;
    case ContainerFormat::Flac:
      ValidateFlac(tracks);
      return Requirement::RandomWrite;
    case ContainerFormat::Mp3:
      ValidateMp3(tracks);
      return Requirement::RandomWrite;
  }
  Fail(ContainerErrorCode::Unsupported, "Unsupported output container");
}

std::unique_ptr<ContainerWriter> OpenWriter(ContainerOutput output, ContainerFormat format,
                                            std::span<const MediaTrack> tracks, ContainerLayout layout) {
  const auto requirement = Validate(format, tracks, layout);
  if (requirement != Requirement::SequentialWrite && !output.Random())
    Fail(ContainerErrorCode::NeedRandomAccess, "This container layout requires random writes");
  if (requirement == Requirement::RandomReadWrite) output.RequireReadable();
  switch (format) {
    case ContainerFormat::Wave:
    case ContainerFormat::Rf64: return OpenWaveWriter(std::move(output), format, tracks);
    case ContainerFormat::Adts: return OpenAdtsWriter(std::move(output), tracks);
    case ContainerFormat::Ogg: return OpenOggWriter(std::move(output), tracks);
    case ContainerFormat::Mp4:
    case ContainerFormat::Mov: return OpenMp4Writer(std::move(output), format, tracks, layout);
    case ContainerFormat::Matroska:
    case ContainerFormat::WebM: return OpenMatroskaWriter(std::move(output), format, tracks);
    case ContainerFormat::Flac: return OpenFlacWriter(std::move(output), tracks);
    case ContainerFormat::Mp3: return OpenMp3Writer(std::move(output), tracks);
  }
  Fail(ContainerErrorCode::Unsupported, "Unsupported output container");
}
} // namespace

MediaMuxer::MediaMuxer(std::unique_ptr<ContainerWriter> writer) : writer_(std::move(writer)) {}
MediaMuxer::MediaMuxer(MediaMuxer&&) noexcept = default;
MediaMuxer& MediaMuxer::operator=(MediaMuxer&&) noexcept = default;
MediaMuxer::~MediaMuxer() = default;

ContainerResult<MediaMuxer::OutputRequirement> MediaMuxer::ValidateOutput(
    ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout) {
  return ContainerCall<OutputRequirement>([&] { return Validate(format, tracks, layout); });
}

ContainerResult<MediaMuxer> MediaMuxer::Open(
    OutputStream output, ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout) {
  return ContainerCall<MediaMuxer>([&] {
    return MediaMuxer(OpenWriter(ContainerOutput(std::move(output)), format, tracks, layout));
  });
}

ContainerResult<MediaMuxer> MediaMuxer::Open(
    RandomAccessStream output, ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout) {
  return ContainerCall<MediaMuxer>([&] {
    return MediaMuxer(OpenWriter(ContainerOutput(std::move(output)), format, tracks, layout));
  });
}

ContainerResult<std::optional<std::uint32_t>> MediaMuxer::TryWrite(const ContainerPacket& packet) {
  auto result = ContainerCall<std::optional<std::uint32_t>>([&] {
    if (!writer_) Fail(ContainerErrorCode::InvalidState, "Container output session is closed");
    const auto index = PacketTrack(writer_->tracks, packet.track_id);
    if (writer_->ended[index]) Fail(ContainerErrorCode::InvalidState, "Container output track has ended");
    const auto size = packet.data.AsBytes().size();
    if (!size) Fail(ContainerErrorCode::InvalidArgument, "Empty container packet");
    if (size > MaxContainerPacket) Fail(ContainerErrorCode::LimitExceeded, "Container packet exceeds 16 MiB");
    if ((packet.pts && !packet.pts->timescale) || (packet.dts && !packet.dts->timescale) ||
        (packet.duration && (!packet.duration->timescale || packet.duration->ticks < 0)) ||
        (packet.discard_padding && !packet.discard_padding->timescale))
      Fail(ContainerErrorCode::InvalidArgument, "Invalid container packet timing");
    if (packet.discard_padding && packet.discard_padding->ticks && !writer_->SupportsPacketTrim())
      Fail(ContainerErrorCode::Unsupported, "Output format cannot retain packet discard padding");
    return writer_->Write(index, packet);
  });
  if (!result.Succeeded() && result.Error().code == ContainerErrorCode::Io) Close();
  return result;
}

ContainerResult<void> MediaMuxer::EndTrack(std::uint32_t track_id, std::optional<std::uint64_t> priming,
                                           std::optional<std::uint64_t> padding) {
  bool sealed = false;
  auto result = ContainerCall<void>([&] {
    if (!writer_) Fail(ContainerErrorCode::InvalidState, "Container output session is closed");
    const auto index = PacketTrack(writer_->tracks, track_id);
    if (writer_->ended[index]) Fail(ContainerErrorCode::InvalidState, "Container output track has ended");
    auto& track = writer_->tracks[index];
    if ((priming || padding) && track.kind != TrackKind::Audio)
      Fail(ContainerErrorCode::InvalidArgument, "Audio trim requires an audio track");
    if ((priming.value_or(0) || padding.value_or(0)) && !writer_->SupportsTrim())
      Fail(ContainerErrorCode::Unsupported, "The output layout cannot store audio trim");
    if ((priming && track.priming_samples && priming != track.priming_samples) ||
        (padding && track.padding_samples && padding != track.padding_samples))
      Fail(ContainerErrorCode::InvalidArgument, "Final audio trim conflicts with the track description");
    if (priming) track.priming_samples = priming;
    if (padding) track.padding_samples = padding;
    writer_->ended[index] = sealed = true;
    writer_->TrackEnded(index);
  });
  if (sealed && !result.Succeeded()) Close();
  return result;
}

ContainerResult<void> MediaMuxer::Finish() {
  return ContainerCall<void>([&] {
    auto writer = std::move(writer_);
    if (!writer) Fail(ContainerErrorCode::InvalidState, "Container output session is closed");
    writer->Finish();
    writer->output.Close();
  });
}

void MediaMuxer::Close() noexcept { writer_.reset(); }
} // namespace huxerui::media
