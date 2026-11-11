#pragma once

/// @file
/// @brief Synchronous container inspection, packet reading, seeking, and packet writing.
///
/// Include <huxerui/media_container.h> and link HuxerUI::Media. Open streams through the SDK.
/// Each session owns its stream. Serialize calls on a caller-managed thread. See docs/media-container.md
/// for the supported format, codec, layout, and stream combinations.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <huxerui/media_format.h>
#include <huxerui/stream.h>

namespace huxerui::media {

namespace detail {
class ContainerReader;
class ContainerWriter;
}

/// @brief Container syntax used to probe input or select output.
enum class ContainerFormat {
  Mp4,      ///< ISO BMFF MP4, including audio-only files and fragmented layouts.
  Mov,      ///< Self-contained QuickTime movie with the `qt  ` major brand.

  Matroska, ///< EBML media with the matroska document type, including audio-only files.
  WebM,     ///< EBML media with the webm document type and its restricted codec mappings.

  Wave,     ///< RIFF WAVE with 32-bit chunk sizes.
  Rf64,     ///< RF64 WAVE with a 64-bit size table.

  Adts,     ///< AAC with an ADTS header on each access unit.
  Ogg,      ///< Mapping-family-zero Opus, with chained logical streams supported on input.
  Flac,     ///< Native FLAC frames following a STREAMINFO metadata block.
  Mp3,      ///< MPEG Layer III audio frames with optional ID3 and Xing information.
};

/// @brief File organization within a container format.
enum class ContainerLayout {
  Default,    ///< Regular layout for the chosen format.
  Fragmented, ///< MP4 initialization followed by movie fragments.
  FastStart,  ///< Regular MP4 output with its movie metadata before the media data.
};

/// @brief Expected extent of a random-access input.
enum class ContainerInputMode {
  Fixed,   ///< A complete input. Truncated boxes are errors.
  Growing, ///< A fragmented MP4 whose provider exposes appended bytes through Size() and ReadAt().
};

/// @brief Media carried by a track.
enum class TrackKind {
  Audio,    ///< Audio samples or compressed audio access units.
  Video,    ///< Coded pictures.
  Subtitle, ///< Timed subtitles.
  Data,     ///< Timed ancillary data.
  Unknown,  ///< The track kind is unavailable.
};

/// @brief Byte representation of packets belonging to a track.
enum class PacketFraming {
  CodecPacket,        ///< Complete codec packet or interleaved, frame-aligned PCM. AAC uses raw access units.
  LengthPrefixedNals, ///< Complete access units with big-endian NAL lengths from the codec configuration.
  AnnexB,             ///< Complete access units with start-code-delimited NAL units.
  ContainerSample,    ///< One source-container sample with container-specific framing.
};

/// @brief Inclusive direction for a time-based seek or index query.
enum class SeekDirection {
  AtOrBefore, ///< Prefer the latest covered point at or before the request.
  AtOrAfter,  ///< Prefer the earliest covered point at or after the request.
};

/// @brief Half-open interval of a track on the exact session presentation timeline.
///
/// Packets outside this interval can still be required for decoding. The interval does not change packet PTS or DTS.
struct MediaPresentationRange {
  /// Inclusive presentation start.
  MediaTimestamp start;
  /// Exclusive presentation end, expressed as a position rather than a duration.
  MediaTimestamp end;

  /// Compares both endpoints as exact rational times.
  bool operator==(const MediaPresentationRange&) const = default;
};

/// @brief Common text tags and front-cover artwork in FLAC, MP3, or single-track audio MP4 (M4A).
///
/// Text is UTF-8. Empty strings represent absent tags. When copying a track to the same container format,
/// its writer emits the supported fields using that format's native metadata blocks.
struct MediaAudioMetadata {
  /// @brief Retained, encoded front-cover image.
  struct Cover {
    /// Image MIME type, for example image/jpeg or image/png.
    std::string mime_type;
    /// Image bytes. Holding this reference keeps the bytes alive after the reader closes.
    BufferReference data;
  };

  /// Track title.
  std::string title;
  /// Performing artist.
  std::string artist;
  /// Album or collection title.
  std::string album;
  /// Track number, which may include the total, such as 2/12. M4A stores the index and total as integers.
  std::string track_number;
  /// Genre text.
  std::string genre;
  /// Release or recording date as written.
  std::string date;
  /// First supported front cover, if present.
  std::optional<Cover> cover;
};

/// @brief Codec configuration and sample-entry properties shared by packets in a track.
struct MediaSampleDescription {
  /// @brief Width-to-height ratio of one coded pixel, before presentation rotation.
  struct PixelAspectRatio {
    /// Positive horizontal spacing in the same units as height.
    std::uint32_t width = 1;
    /// Positive vertical spacing in the same units as width.
    std::uint32_t height = 1;

    /// Compares both stored spacings.
    bool operator==(const PixelAspectRatio&) const = default;
  };

  /// @brief A supported MP4 or MOV visual sample-entry extension retained for packet-copy output.
  struct VideoExtension {
    /// Four-character box type, such as colr, mdcv, or dvcC.
    std::string type;
    /// Exact box payload after its size and type. Codec initialization, crop, and aspect use separate fields.
    Bytes payload;

    /// Compares the box type and payload bytes.
    bool operator==(const VideoExtension&) const = default;
  };

  /// Codec of packets using this description.
  CodecId codec = CodecId::Unknown;
  /// Representation of packet bytes.
  PacketFraming framing = PacketFraming::CodecPacket;
  /// Codec configuration in the representation specified by the mapping, such as the payload of an hvcC box.
  /// ALAC uses the 24-byte ALACSpecificConfig without the MP4 box header or version and flags. MOV timecode
  /// and timed metadata use the sample-entry payload after the reserved fields and data-reference index.
  Bytes initialization_data;
  /// Audio samples per channel per second.
  std::optional<std::uint32_t> sample_rate;
  /// Audio channel count.
  std::optional<std::uint32_t> channel_count;
  /// Coded luma width in pixels.
  std::optional<std::uint32_t> coded_width;
  /// Coded luma height in pixels.
  std::optional<std::uint32_t> coded_height;
  /// Known visible area in coded luma pixels.
  std::optional<VideoRectangle> visible;
  /// Four-character ISO BMFF or QuickTime sample entry for mappings that distinguish variants.
  /// HEVC retains hvc1 or hev1. ProRes retains its profile entry, such as apch. MOV data tracks use tmcd or mebx.
  /// Empty selects hvc1 for HEVC. ProRes output requires an explicit profile entry.
  std::string sample_entry;
  /// Explicit pixel spacing from a video sample entry. Absence leaves the source's aspect unspecified.
  std::optional<PixelAspectRatio> pixel_aspect_ratio = {};
  /// Supported color and HDR extension payloads from MP4 or MOV. Preserve them when copying compatible
  /// compressed samples. Supply metadata for the new bitstream when encoding different pictures.
  std::vector<VideoExtension> video_extensions = {};
};

/// @brief One track and its available sample descriptions.
///
/// A track ID is nonzero and identifies one track in its session. Packets and decode-start events
/// reference entries in descriptions by their one-based index. A reader can append new configurations
/// as it discovers them. An existing index keeps its meaning for the life of the session.
struct MediaTrack {
  /// @brief One rate-one edit on the track's presentation timeline.
  ///
  /// Entries are laid out in order from movie time zero. A missing media start represents an empty edit.
  struct Edit {
    /// Positive length on the presentation timeline.
    MediaTimestamp duration;
    /// Start on the packet timeline. Media intervals are forward and nonoverlapping.
    std::optional<MediaTimestamp> media_start;

    /// Compares the edit length and source position as rational times.
    bool operator==(const Edit&) const = default;
  };

  /// @brief A supported QuickTime track association.
  struct Reference {
    /// Four-character reference type, such as tmcd or cdsc.
    std::string type;
    /// ID of another track in the same movie.
    std::uint32_t track_id = 0;

    /// Compares the reference type and target.
    bool operator==(const Reference&) const = default;
  };

  /// Session-local track identity.
  std::uint32_t id = 0;
  /// Media kind.
  TrackKind kind = TrackKind::Unknown;
  /// Known media clock or requested output tick rate. Matroska and WebM expose their Segment clock separately.
  std::optional<std::uint32_t> timescale;
  /// Packet configurations in one-based index order. Output requires at least one entry.
  std::vector<MediaSampleDescription> descriptions;
  /// Known initial audio trim in samples per channel. MP4 and MOV report authored cuts in edits.
  std::optional<std::uint64_t> priming_samples;
  /// Known trailing audio trim in samples per channel.
  std::optional<std::uint64_t> padding_samples;
  /// Clockwise presentation rotation. Packet bytes keep their coded orientation.
  VideoRotation rotation = VideoRotation::None;
  /// Known nonnegative duration of data to decode before presenting output after seeking.
  /// Absence leaves the preroll unspecified. Matroska and WebM can retain it in the track header.
  std::optional<MediaTimestamp> seek_preroll = {};
  /// Ordered rate-one MP4 or MOV edits. An empty list maps media times directly to presentation times.
  /// Pass the edits and the original packet times together to preserve the authored presentation.
  /// Fragmented output needs the list at open and requires nonnegative packet DTS.
  std::vector<Edit> edits;
  /// Supported MOV track associations. Include their target tracks when copying them to a new movie.
  std::vector<Reference> references;
  /// Default packet duration declared by Matroska or WebM. Absence means no default was declared.
  /// Pass it through when copying these tracks. An explicit packet duration takes precedence, followed by
  /// the codec's intrinsic duration, when known, then this default.
  std::optional<MediaTimestamp> default_duration;
};

/// @brief One interval retained when mapping a decoded media interval through a track edit list.
struct MediaPresentationSlice {
  /// Start of the retained interval on the packet media timeline.
  MediaTimestamp media_start;
  /// Mapped interval with the same duration, on the presentation timeline.
  MediaPresentationRange presentation;
};

/// @brief A packet on its track media timeline.
///
/// Packets arrive in decode order within each track. Absent timing remains distinct from zero.
/// Bytes returned by Read() are retained and remain readable after subsequent reads, seeks, and Close().
struct ContainerPacket {
  /// Identity of the track that describes these bytes.
  std::uint32_t track_id = 0;
  /// Retained, nonempty packet bytes.
  BufferReference data;
  /// Media PTS before applying track edits and trim.
  std::optional<MediaTimestamp> pts;
  /// Decode time on the same timeline.
  std::optional<MediaTimestamp> dts;
  /// Nonnegative packet duration on the container timeline.
  std::optional<MediaTimestamp> duration;
  /// Known container sync flag. Absence means unavailable.
  std::optional<bool> sync_sample;
  /// Optional audio trim within this packet. A negative value trims the beginning, a positive value trims the end.
  /// This duration does not shift PTS and is separate from trim declared on the track.
  std::optional<MediaTimestamp> discard_padding = {};
  /// One-based index into the descriptions of the identified track.
  std::uint32_t sample_description_index = 1;
};

/// @brief A covered index candidate or actual seek anchor on a reference track.
struct SeekPoint {
  /// @brief Evidence for using this point as a decoding entry.
  enum class Kind {
    SyncMarked,   ///< The container marks this sample as a sync point.
    Independent,  ///< The mapping or packet syntax establishes independent decoding with matching configuration.
    NeedsPreroll, ///< The point requires preceding samples to prepare the decoder.
  };

  /// Reference track ID.
  std::uint32_t track_id = 0;
  /// Actual anchor presentation time after applying edits and known initial audio trim.
  MediaTimestamp presentation_time;
  /// Available evidence for decoding from this point.
  Kind kind = Kind::SyncMarked;
  /// Known anchor byte offset relative to the beginning of the container input.
  std::optional<std::uint64_t> byte_offset;
  /// Media PTS of the first reference-track packet needed to decode from the anchor.
  /// It may precede the presented samples. Other selected tracks can start at different times.
  MediaTimestamp decode_start_pts;
};

/// @brief Begins a continuous decoding sequence on a selected track.
struct TrackDecodeStart {
  /// Track whose next packet uses this configuration.
  std::uint32_t track_id;
  /// One-based index into that track's descriptions.
  std::uint32_t sample_description_index;
  /// Known lower bound for audible samples in this sequence, in media PTS.
  /// This is an absolute bound and does not move when seeking into the sequence.
  std::optional<MediaTimestamp> first_audible_pts;
};

/// @brief Packet data or the start of a decoding sequence preceding its first packet.
using DemuxItem = std::variant<ContainerPacket, TrackDecodeStart>;

/// @brief Portable categories for container failures.
enum class ContainerErrorCode {
  UnrecognizedFormat, ///< The input could not be identified within the probe budget.
  Unsupported,        ///< The format, layout, or codec mapping is unavailable.
  InvalidArgument,    ///< A caller-supplied track, packet, or stream violates the contract.
  InvalidState,       ///< The session is closed, moved from, failed, or the track has ended.
  InvalidData,        ///< The input is malformed or truncated.
  MissingTiming,      ///< Writing requires timing that cannot be inferred safely.
  NeedRandomAccess,   ///< The operation requires a random-access endpoint.
  IndexUnavailable,   ///< Available index coverage cannot answer the time query.
  LimitExceeded,      ///< A bounded parser, packet, or index limit was exceeded.
  Io,                 ///< An underlying SDK stream operation failed.
};

/// @brief A container failure with its optional SDK I/O cause.
struct ContainerError {
  /// Stable category for handling the failure.
  ContainerErrorCode code;
  /// Diagnostic context for logs.
  std::string message;
  /// Original stream error when code is Io.
  std::optional<IoError> io_error;
};

/// @brief Success or failure of a container operation.
/// @tparam T Owned success value, or void for success without a value.
template <class T> using ContainerResult = Result<T, ContainerError>;

/// @brief Intersects a decoded interval with the track edits and maps the retained pieces.
/// @param edits Ordered rate-one edits, or an empty span for the identity mapping.
/// @param media_begin Inclusive media PTS before applying the edits.
/// @param media_end Exclusive media PTS.
/// @param output Receives slices in presentation order. Storage can be reused across calls.
/// @return Success, including an empty result for an excluded interval, or a timing error.
/// Equivalent rational timestamps are mapped on the same exact timeline.
ContainerResult<void> MapToPresentation(std::span<const MediaTrack::Edit> edits, MediaTimestamp media_begin,
                                        MediaTimestamp media_end, std::vector<MediaPresentationSlice>& output);

/// @brief Owns a synchronous, move-only container reading session.
///
/// The metadata getters throw std::logic_error if the session is closed, moved from, or failed.
class MediaDemuxer final {
public:
  /// Transfers the session and its stream.
  MediaDemuxer(MediaDemuxer&&) noexcept;
  /// Releases the current session and takes the supplied one.
  MediaDemuxer& operator=(MediaDemuxer&&) noexcept;
  /// Releases the input.
  ~MediaDemuxer();

  /// @brief Probes a sequential input starting at its current position.
  /// @param input Stream transferred to the operation, including on failure.
  /// @param hint Optional expected container syntax. Header bytes still undergo validation.
  /// @return A session or the probe or open error. Layouts requiring positioning report NeedRandomAccess.
  static ContainerResult<MediaDemuxer> Open(InputStream input, std::optional<ContainerFormat> hint = {});

  /// @brief Probes a readable random-access input starting at offset zero.
  /// @param input Stream transferred to the operation, including on failure.
  /// @param hint Optional expected container syntax.
  /// @return A session for a fixed input, or the probe or open error.
  static ContainerResult<MediaDemuxer> Open(RandomAccessStream input, std::optional<ContainerFormat> hint = {});

  /// @brief Probes a readable random-access input with an explicit extent policy.
  /// @param input Stream transferred to the operation, including on failure.
  /// @param hint Optional expected container syntax.
  /// @param mode Growing exposes only complete fragments. A successful empty Read() can be retried after an append.
  /// The provider must report a current size and the existing prefix must stay unchanged.
  /// @return A session or the probe or open error.
  static ContainerResult<MediaDemuxer> Open(RandomAccessStream input, std::optional<ContainerFormat> hint,
                                           ContainerInputMode mode);

  /// @return The recognized container format. Requires a live session.
  [[nodiscard]] ContainerFormat Format() const;
  /// @return The recognized layout. Requires a live session.
  [[nodiscard]] ContainerLayout Layout() const;
  /// @return The Matroska or WebM Segment tick duration, including the format default when omitted in the file.
  /// Other formats return std::nullopt. Requires a live session and preserves the read position.
  std::optional<std::chrono::nanoseconds> EbmlTimestampScale() const;
  /// @return Tracks and their known sample descriptions, borrowed until the next session mutation.
  [[nodiscard]] std::span<const MediaTrack> Tracks() const;
  /// @return File tags and cover art, if recognized. Artwork retains its storage after closing.
  std::optional<MediaAudioMetadata> AudioMetadata() const;
  /// @return Known presentation start on the session timeline. Requires a live session.
  [[nodiscard]] std::optional<MediaTimestamp> StartTime() const;
  /// @return Known presentation duration. Requires a live session.
  /// MP4 and MOV include declared empty edits at the end of the timeline.
  [[nodiscard]] std::optional<MediaTimestamp> Duration() const;

  /// @brief Returns the bounds of the samples presented by the track's supported edits.
  /// @param track_id Track identity in this session.
  /// @return A half-open interval, or std::nullopt for an unknown track or unavailable sample timing.
  /// Equal endpoints mean no samples intersect the edit. Requires a live session and does not move the read
  /// position or build an index. MP4 and MOV supply these bounds from sample tables or verified fragments.
  /// The bounds can span gaps in a multi-segment edit list. Fragmented input can initially report partial coverage.
  /// Native FLAC and MP3 supply them when the total sample count and any available gapless trim are known.
  std::optional<MediaPresentationRange> PresentationRange(std::uint32_t track_id) const;

  /// @brief Selects tracks for subsequent reads at the current position.
  /// @param tracks Known track IDs. An empty span restores selection of all tracks.
  /// @return Success or an error. Selection is preserved across seeks.
  ContainerResult<void> SelectTracks(std::span<const std::uint32_t> tracks);

  /// @brief Reads a selected packet or a decoding boundary.
  /// @return The next item, a successful empty value at EOF, or a parsing or I/O error.
  /// TrackDecodeStart precedes the first packet, a configuration change, a new Ogg link, and the
  /// first packet after a seek or after selecting a previously excluded track. Look up the index in Tracks().
  ContainerResult<std::optional<DemuxItem>> Read();

  /// @brief Queries a covered point while preserving the read position.
  /// Random-access fragmented MP4 extends its bounded index as needed. Full coverage establishes the sample bounds.
  /// A validated embedded index can identify a fragment directly. Sized random-access Matroska and WebM
  /// can scan Cluster headers and Block prefixes when the reference track has no usable Cues.
  /// @param track_id Reference track.
  /// @param time Target presentation time.
  /// @param direction Inclusive search direction.
  /// @return A point, a successful empty value if the covered range has no match, or an error.
  ContainerResult<std::optional<SeekPoint>> FindSeekPoint(
      std::uint32_t track_id, MediaTimestamp time, SeekDirection direction = SeekDirection::AtOrBefore);

  /// @brief Positions selected tracks using an anchor on the reference track.
  /// @param track_id Reference track.
  /// @param time Target presentation time.
  /// @param direction Inclusive search direction.
  /// @return The actual anchor, a successful empty value if no matching anchor exists, or an error.
  /// A successful empty value preserves the read position. The next packet can precede the anchor for preroll.
  ContainerResult<std::optional<SeekPoint>> Seek(
      std::uint32_t track_id, MediaTimestamp time, SeekDirection direction = SeekDirection::AtOrBefore);

  /// @brief Releases the input and closes the session.
  /// @return Success or a stream close error. Repeated close is safe.
  ContainerResult<void> Close();

private:
  explicit MediaDemuxer(std::unique_ptr<detail::ContainerReader> reader);
  detail::ContainerReader& Require() const;
  std::unique_ptr<detail::ContainerReader> reader_;
};

/// @brief Output layout, optional file-level audio tags, and container timing.
struct MediaMuxerOptions {
  /// Requested output layout for the chosen container.
  ContainerLayout layout = ContainerLayout::Default;
  /// Tags and cover art for native FLAC, MP3, or regular single-audio MP4.
  std::optional<MediaAudioMetadata> audio_metadata;
  /// Positive nanoseconds per Matroska or WebM Segment tick. The default is one millisecond.
  /// To copy the source clock, pass MediaDemuxer::EbmlTimestampScale(). Other output formats require absence.
  std::optional<std::chrono::nanoseconds> ebml_timestamp_scale;
};

/// @brief Owns a synchronous, move-only container writing session.
///
/// Submit packets in decode order within each track. Finish() completes the container and closes the stream.
/// Dropping a session before successful Finish() can leave partial output.
class MediaMuxer final {
public:
  /// @brief Minimum stream ability needed by a validated output configuration.
  enum class OutputRequirement {
    SequentialWrite, ///< Supports sequential writing.
    RandomWrite,     ///< Supports writing at absolute offsets, including backpatching.
    RandomReadWrite, ///< Supports reading and writing at absolute offsets for final layout changes.
  };

  /// Transfers the session and its stream.
  MediaMuxer(MediaMuxer&&) noexcept;
  /// Abandons the current output and takes the supplied session.
  MediaMuxer& operator=(MediaMuxer&&) noexcept;
  /// Releases the output without finishing it.
  ~MediaMuxer();

  /// @brief Validates a container and track combination without opening or writing a destination.
  /// @param format Target container format.
  /// @param tracks One entry per track ID, with the descriptions referenced by its packets.
  /// @param options Target layout, file tags, and container timing.
  /// @return The minimum required I/O ability or the configuration error.
  static ContainerResult<OutputRequirement> ValidateOutput(
      ContainerFormat format, std::span<const MediaTrack> tracks, const MediaMuxerOptions& options = {});

  /// @brief Begins output on a fresh sequential stream at its beginning.
  /// @param output Stream transferred to the operation, including on failure.
  /// @param format Target container format.
  /// @param tracks Track descriptions copied by the session.
  /// @param options Target layout, file tags, and container timing.
  /// @return A session or an error. Layouts requiring backpatching report NeedRandomAccess.
  static ContainerResult<MediaMuxer> Open(
      OutputStream output, ContainerFormat format, std::span<const MediaTrack> tracks,
      const MediaMuxerOptions& options = {});

  /// @brief Begins output at offset zero on an empty, writable random-access stream.
  /// @param output Stream transferred to the operation, including on failure.
  /// @param format Target container format.
  /// @param tracks Track descriptions copied by the session.
  /// @param options Target layout, file tags, and container timing.
  /// @return A session or an error. FastStart requires an SDK stream opened in ReadWrite mode.
  static ContainerResult<MediaMuxer> Open(
      RandomAccessStream output, ContainerFormat format, std::span<const MediaTrack> tracks,
      const MediaMuxerOptions& options = {});

  /// @brief Submits a complete packet for the given track.
  /// @param packet Bytes and timing borrowed until return. The session owns any retained copy.
  /// @return Success with no ID after acceptance. Success with a track ID leaves the packet unconsumed.
  /// Supply that track's next packet or end the track, then retry. A write failure can leave partial output.
  ContainerResult<std::optional<std::uint32_t>> TryWrite(const ContainerPacket& packet);

  /// @brief Ends a track so remaining tracks can continue through interleaving.
  /// @param track_id Output track ID.
  /// @param priming_samples Known leading audio trim, or absent to keep the existing value.
  /// @param padding_samples Known trailing audio trim, or absent to keep the existing value.
  /// @return Success or an error. A supplied trim value must be representable in the target layout.
  ContainerResult<void> EndTrack(std::uint32_t track_id, std::optional<std::uint64_t> priming_samples = {},
                                 std::optional<std::uint64_t> padding_samples = {});

  /// @brief Ends remaining tracks, writes final tables, and finalizes the SDK output.
  /// @return Success after the output closes, or an error. A failed write or close can leave partial bytes.
  ContainerResult<void> Finish();

  /// @brief Abandons unfinished output and releases the stream. Repeated close is safe.
  void Close() noexcept;

private:
  explicit MediaMuxer(std::unique_ptr<detail::ContainerWriter> writer);
  std::unique_ptr<detail::ContainerWriter> writer_;
};

} // namespace huxerui::media
