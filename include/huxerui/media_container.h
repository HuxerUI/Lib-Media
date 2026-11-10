#pragma once

/// @file
/// @brief Synchronous container inspection, packet reading, seeking, and packet writing.
///
/// Include <huxerui/media_container.h> and link HuxerUI::Media. Open streams through the SDK.
/// Each session owns its stream. Serialize calls on a caller-managed thread. See docs/media-container.md
/// for the supported format, codec, layout, and stream combinations.

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
  Ogg,      ///< A single Ogg logical stream with mapping-family-zero Opus.
  Flac,     ///< Native FLAC frames following a STREAMINFO metadata block.
  Mp3,      ///< MPEG Layer III audio frames with optional ID3 and Xing information.
};

/// @brief File organization within a container format.
enum class ContainerLayout {
  Default,    ///< Regular layout for the chosen format.
  Fragmented, ///< MP4 initialization followed by movie fragments.
  FastStart,  ///< Regular MP4 output with its movie metadata before the media data.
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

/// @brief Track description shared by the demuxer and muxer.
///
/// A track ID is nonzero and unique in its session. Callers may reuse an input track ID in an output session.
/// A description returned by Read() applies to subsequent packets with the same ID until replaced.
struct MediaTrack {
  /// Session-local track identity.
  std::uint32_t id = 0;
  /// Media kind.
  TrackKind kind = TrackKind::Unknown;
  /// Track coding.
  CodecId codec = CodecId::Unknown;
  /// Representation of packet bytes.
  PacketFraming framing = PacketFraming::CodecPacket;
  /// Codec configuration in the representation specified by the mapping, such as the payload of an hvcC box.
  Bytes initialization_data;
  /// Known track clock or requested output tick rate.
  std::optional<std::uint32_t> timescale;

  /// Audio samples per channel per second.
  std::optional<std::uint32_t> sample_rate;
  /// Audio channel count.
  std::optional<std::uint32_t> channel_count;
  /// Known leading trim in samples per channel.
  std::optional<std::uint64_t> priming_samples;
  /// Known trailing trim in samples per channel, relative to the reported packet timeline.
  std::optional<std::uint64_t> padding_samples;

  /// Coded luma width in pixels.
  std::optional<std::uint32_t> coded_width;
  /// Coded luma height in pixels.
  std::optional<std::uint32_t> coded_height;
  /// Known visible area in coded luma pixels.
  std::optional<VideoRectangle> visible;
  /// Clockwise presentation rotation of the visible image. Packet bytes keep their coded orientation.
  VideoRotation rotation = VideoRotation::None;
  /// Four-character ISO BMFF or QuickTime sample entry for mappings that distinguish variants.
  /// HEVC retains hvc1 or hev1. ProRes retains its profile entry, such as apch.
  /// Empty selects hvc1 for HEVC. ProRes output requires an explicit profile entry.
  std::string sample_entry;
  /// Known nonnegative duration of data to decode before using the output after seeking.
  /// Absence leaves the preroll unspecified. Matroska and WebM can retain this value in the track header.
  std::optional<MediaTimestamp> seek_preroll = {};
};

/// @brief A packet on a container session's common timeline.
///
/// Packets arrive in decode order within each track. Absent timing remains distinct from zero.
/// Bytes returned by Read() are retained and remain readable after subsequent reads, seeks, and Close().
struct ContainerPacket {
  /// Identity of the track that describes these bytes.
  std::uint32_t track_id = 0;
  /// Retained, nonempty packet bytes.
  BufferReference data;
  /// Presentation time before applying track trim.
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
  /// Actual anchor presentation time.
  MediaTimestamp time;
  /// Available evidence for decoding from this point.
  Kind kind = Kind::SyncMarked;
  /// Known byte offset relative to the beginning of the container input.
  std::optional<std::uint64_t> byte_offset;
  /// Known earlier time from which to start reading for preroll.
  std::optional<MediaTimestamp> decode_start;
};

/// @brief Packet data or the new track description preceding dependent packets.
using DemuxItem = std::variant<ContainerPacket, MediaTrack>;

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
  /// @return A session or the probe or open error.
  static ContainerResult<MediaDemuxer> Open(RandomAccessStream input, std::optional<ContainerFormat> hint = {});

  /// @return The recognized container format. Requires a live session.
  [[nodiscard]] ContainerFormat Format() const;
  /// @return The recognized layout. Requires a live session.
  [[nodiscard]] ContainerLayout Layout() const;
  /// @return Current descriptions, borrowed until the next session mutation. Requires a live session.
  [[nodiscard]] std::span<const MediaTrack> Tracks() const;
  /// @return Known presentation start on the session timeline. Requires a live session.
  [[nodiscard]] std::optional<MediaTimestamp> StartTime() const;
  /// @return Known presentation duration. Requires a live session.
  [[nodiscard]] std::optional<MediaTimestamp> Duration() const;

  /// @brief Selects tracks for subsequent reads at the current position.
  /// @param tracks Known track IDs. An empty span restores selection of all tracks.
  /// @return Success or an error. Selection is preserved across seeks.
  ContainerResult<void> SelectTracks(std::span<const std::uint32_t> tracks);

  /// @brief Reads one selected packet or a track description.
  /// @return The next item, a successful empty value at EOF, or a parsing or I/O error.
  /// Updated descriptions precede their packets. After Seek(), descriptions precede the first packets read.
  ContainerResult<std::optional<DemuxItem>> Read();

  /// @brief Queries a covered point while preserving the read position.
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
  /// @param tracks Track descriptions with unique IDs.
  /// @param layout Target layout.
  /// @return The minimum required I/O ability or the configuration error.
  static ContainerResult<OutputRequirement> ValidateOutput(
      ContainerFormat format, std::span<const MediaTrack> tracks, ContainerLayout layout = ContainerLayout::Default);

  /// @brief Begins output on a fresh sequential stream at its beginning.
  /// @param output Stream transferred to the operation, including on failure.
  /// @param format Target container format.
  /// @param tracks Track descriptions copied by the session.
  /// @param layout Target layout.
  /// @return A session or an error. Layouts requiring backpatching report NeedRandomAccess.
  static ContainerResult<MediaMuxer> Open(
      OutputStream output, ContainerFormat format, std::span<const MediaTrack> tracks,
      ContainerLayout layout = ContainerLayout::Default);

  /// @brief Begins output at offset zero on an empty, writable random-access stream.
  /// @param output Stream transferred to the operation, including on failure.
  /// @param format Target container format.
  /// @param tracks Track descriptions copied by the session.
  /// @param layout Target layout.
  /// @return A session or an error. FastStart requires an SDK stream opened in ReadWrite mode.
  static ContainerResult<MediaMuxer> Open(
      RandomAccessStream output, ContainerFormat format, std::span<const MediaTrack> tracks,
      ContainerLayout layout = ContainerLayout::Default);

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
