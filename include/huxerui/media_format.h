#pragma once

/// @file
/// @brief Shared codec identifiers, exact media timestamps, compressed access units, and decoded media frames.
///
/// Include <huxerui/media_format.h> and link HuxerUI::Media. These values can be used in headless code.
/// Use <huxerui/media_codec.h> to decode compressed packets or encode PCM and video through bounded, headless sessions.
/// When constructing frames yourself, call Validate() before using their format, timing, and storage.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <huxerui/data.h>

namespace huxerui::media {

namespace detail {
struct PlatformVideoStorage;
struct VideoFrameAccess;
}

/// @brief An exact media timeline position or duration expressed as ticks / timescale seconds.
///
/// Negative positions can represent preroll. Fields that carry a duration require nonnegative ticks.
/// Use std::optional for absent timing and a positive timescale for every present value.
/// @code{.cpp}
/// const huxerui::media::MediaTimestamp pts{9000, 90000};
/// const auto microseconds = pts.Rescale(1000000); // Contains {100000, 1000000}.
/// @endcode
struct MediaTimestamp {
  /// Signed number of ticks relative to the stream timeline origin, or the length of a duration.
  std::int64_t ticks = 0;
  /// Positive number of ticks per second.
  std::uint32_t timescale = 1;

  /// @brief Converts this value to a different tick rate using integer arithmetic.
  /// @param scale Positive number of ticks per second in the result.
  /// @return The nearest timestamp at scale, rounding ties away from zero. Returns std::nullopt if either
  /// timescale is zero or the rounded tick count would overflow std::int64_t.
  [[nodiscard]] std::optional<MediaTimestamp> Rescale(std::uint32_t scale) const noexcept;

  /// @brief Compares two exact times without rounding them to a common tick rate.
  /// @param other Time on the same timeline, or another duration when comparing durations.
  /// @return -1 if this time is earlier or shorter, 0 if equal, and 1 if later or longer.
  /// Returns std::nullopt when either timescale is zero.
  [[nodiscard]] std::optional<int> Compare(MediaTimestamp other) const noexcept;

  /// @brief Tests equality of the exact rational values, including equivalent fractions with different timescales.
  /// @param other Time to compare with this value.
  /// @return True if both timescales are positive and Compare(other) yields zero.
  bool operator==(MediaTimestamp other) const noexcept { return Compare(other) == 0; }
};

/// @brief PCM sample representations in native byte order.
enum class AudioSampleFormat {
  /// Signed 16-bit integer PCM with two bytes per sample.
  S16,
  /// 32-bit floating-point PCM with four bytes per sample.
  F32
};

/// @brief Placement of PCM channel samples within an AudioFrame.
enum class AudioPacking {
  /// One plane. Each sample position stores the samples of all channels in channel order.
  Interleaved,
  /// One plane per channel. Each plane stores that channel's consecutive samples.
  Planar
};

/// @brief Speaker layout and sample order for the supported channel counts.
enum class AudioChannelLayout {
  /// One center channel.
  Mono,
  /// Two channels in left, right order.
  Stereo
};

/// @brief PCM sample rate, representation, packing, and ordered channel layout.
///
/// Derive channel count and bytes per channel sample with ChannelCount() and BytesPerSample(). Set a positive
/// sample_rate before using a default-constructed format in an AudioFrame, AudioDecoderConfig, or AudioEncoderConfig.
/// AudioEncoderConfig additionally requires S16 representation and interleaved packing.
struct AudioFormat {
  /// Positive sample rate in samples per second per channel. The default zero is unset.
  std::uint32_t sample_rate = 0;
  /// Signed 16-bit integer or 32-bit floating-point samples.
  AudioSampleFormat sample_format = AudioSampleFormat::F32;
  /// Interleaved has one plane; planar has one plane per channel.
  AudioPacking packing = AudioPacking::Interleaved;
  /// Channel order, limited to mono and stereo in this version.
  AudioChannelLayout channel_layout = AudioChannelLayout::Stereo;

  /// @brief Derives the number of channels from channel_layout.
  /// @return One for Mono, two for Stereo, or zero for an unrecognized layout value.
  [[nodiscard]] std::uint32_t ChannelCount() const noexcept;

  /// @brief Derives the byte width of one channel sample from sample_format.
  /// @return Two for S16, four for F32, or zero for an unrecognized representation value.
  [[nodiscard]] std::uint32_t BytesPerSample() const noexcept;

  /// @brief Compares all four PCM format fields.
  /// @return True if the rate, sample representation, packing, and channel layout are equal, otherwise false.
  bool operator==(const AudioFormat&) const = default;
};

/// @brief Successful outcomes of submitting media to a bounded session.
enum class SubmitStatus {
  /// The session accepted the entire input and owns the bytes needed for asynchronous work.
  Accepted,
  /// The session consumed no input. Retry when startup, reset, or queue capacity allows it.
  WouldBlock
};

/// @brief Coding of a media stream, independent of packet framing and platform support.
enum class CodecId {
  Unknown,  ///< No recognized coding.

  H264,     ///< AVC coded pictures.
  Hevc,     ///< High Efficiency Video Coding.
  Vp8,      ///< VP8 coded pictures.
  Vp9,      ///< VP9 coded pictures.
  ProRes,   ///< Apple ProRes coded pictures. The sample entry identifies the profile.

  Aac,      ///< Advanced Audio Coding.
  Alac,     ///< Apple Lossless Audio Codec. The container API supports packet copy.
  Opus,     ///< Opus coded audio. Trim uses the 48 kHz decoded sample grid.
  Flac,     ///< Complete native FLAC frames.
  Mp3,      ///< Complete MPEG Layer III frames.

  PcmU8,    ///< Unsigned eight-bit PCM.
  PcmS16Le, ///< Signed 16-bit little-endian PCM.
  PcmS24Le, ///< Signed packed 24-bit little-endian PCM.
  PcmS32Le, ///< Signed 32-bit little-endian PCM.
  PcmF32Le, ///< IEEE float 32-bit little-endian PCM.

  PcmS16Be, ///< Signed 16-bit big-endian PCM.
  PcmS24Be, ///< Signed packed 24-bit big-endian PCM.
  PcmS32Be, ///< Signed 32-bit big-endian PCM.
  PcmF32Be, ///< IEEE float 32-bit big-endian PCM.
};

/// @brief A complete compressed access unit with timing on the source's media timeline.
///
/// Supply one raw AAC access unit or one complete length-prefixed AVC or HEVC picture access unit per packet. Keep data
/// address-stable and unchanged throughout TrySubmit(). BufferReference retains the backing owner, which may still
/// be mutable. Accepted packets are copied before TrySubmit() returns, so the caller can then reuse the source bytes.
/// Keep a packet available for retry when submission returns SubmitStatus::WouldBlock.
/// Supplied timing must fit in signed 64-bit microseconds after rescaling. A platform adapter may impose a smaller range.
///
/// AudioEncoder and VideoEncoder emit immutable bytes that remain readable after Reset(), Close(), and cleanup
/// completion.
/// Retaining a packet, its data, or an arbitrary slice keeps its full output reservation in the originating encoder,
/// including across resets. Release all byte references to return that reservation.
/// @see AudioDecoder::TrySubmit(), VideoDecoder::TrySubmit(), AudioEncoder::TryRead(), VideoEncoder::TryRead()
struct EncodedPacket {
  /// Nonempty access-unit bytes in the representation selected by the codec configuration.
  BufferReference data;
  /// Required presentation timestamp with a positive timescale. Negative preroll is allowed.
  /// Video packets can arrive with reordered presentation timestamps.
  MediaTimestamp pts;
  /// Optional decode timestamp on the same timeline. Submit packets in decode order whether or not DTS is present.
  std::optional<MediaTimestamp> dts;
  /// Optional nonnegative duration with a positive timescale. An absent duration and a zero duration are distinct.
  std::optional<MediaTimestamp> duration;
  /// Marks a random-access unit. The first video packet after Create() or Reset() must contain an IDR and set this flag.
  /// AudioEncoder output always sets this flag; VideoEncoder derives it from actual IDR syntax.
  bool key_frame = false;
};

/// @brief Retained PCM samples for a contiguous interval of media time.
///
/// Decoder output owns immutable CPU storage that remains readable after Reset(), Close(), and cleanup completion.
/// Copies share the plane buffers. Retaining a plane or any slice of it holds the frame's output reservation in the
/// originating decoder across resets. Release all plane references to return that reservation.
/// For application-provided planes, keep their storage stable and synchronize writes with readers.
/// AudioEncoder copies the used PCM bytes on Accepted; its input must match AudioEncoderConfig::input and follow
/// the previously accepted frame on the configured sample grid.
/// @see AudioFormat, BufferReference, AudioEncoder::TrySubmit()
struct AudioFrame {
  /// Complete format of these samples.
  AudioFormat format;
  /// Presentation time of the first sample frame, before any application trimming. Decoder output preserves the
  /// associated packet's exact rational PTS.
  MediaTimestamp pts;
  /// Positive sample count per channel. Each position contributes one sample from every channel.
  std::uint32_t sample_count = 0;
  /// One packed plane for interleaved audio or one contiguous plane per channel for planar audio.
  /// Each plane starts at its first sample. Keep a BufferReference alive while using its borrowed AsBytes() span.
  std::vector<BufferReference> planes;

  /// @brief Computes the exact duration from sample_count and format.sample_rate.
  /// @return MediaTimestamp{sample_count, format.sample_rate}. A zero sample rate produces an invalid timescale.
  [[nodiscard]] MediaTimestamp Duration() const noexcept { return {sample_count, format.sample_rate}; }

  /// @brief Checks the PCM format, timing, and addressable sample range of each plane.
  ///
  /// The format and PTS must be valid and sample_count must be positive. Each plane must start at an address
  /// aligned to format.BytesPerSample(). The minimum byte count is sample_count times format.BytesPerSample()
  /// times the number of channels in that plane. This calculation must fit in std::size_t. Extra bytes after the
  /// samples are allowed.
  /// @return True if the format, sample count, PTS, plane count, alignment, and sizes satisfy those requirements.
  [[nodiscard]] bool Validate() const noexcept;
};

/// @brief Pixel layout of a video frame.
///
/// Luma uses the coded width and height. Chroma uses half each coded dimension, rounded up.
enum class VideoPixelFormat {
  /// Three planes in Y, U, V order, with one byte per sample in each plane.
  I420,
  /// A Y plane followed by a chroma plane containing consecutive U, V byte pairs.
  NV12,
  /// Two planes with the same component order as NV12. Each component occupies a little-endian 16-bit word
  /// with its ten-bit value in the most significant bits.
  P010,
  /// A platform image whose pixel layout is opaque to the caller.
  Opaque
};

/// @brief Selects the storage used to exchange uncompressed video with a codec.
enum class VideoStorage {
  /// CPU-readable YUV planes.
  Cpu,
  /// A retained, GPU-accessible platform image. Probe the requested codec and layout.
  Platform
};

/// @brief Color primaries describing the coded samples.
enum class ColorPrimaries {
  /// Primaries were unavailable or could not be mapped to a supported value.
  Unknown,
  /// BT.709 primaries.
  BT709,
  /// Primaries associated with 525-line BT.601 video.
  BT601_525,
  /// Primaries associated with 625-line BT.601 video.
  BT601_625,
  /// BT.2020 primaries.
  BT2020
};

/// @brief Transfer characteristic for the coded color values.
enum class ColorTransfer {
  /// Transfer characteristic was unavailable or could not be mapped.
  Unknown,
  /// BT.709 transfer characteristic.
  BT709,
  /// sRGB transfer characteristic.
  SRGB,
  /// Linear-light values.
  Linear,
  /// Perceptual Quantizer transfer characteristic.
  PQ,
  /// Hybrid Log-Gamma transfer characteristic.
  HLG
};

/// @brief Matrix coefficients for conversion between YUV and RGB.
enum class ColorMatrix {
  /// Matrix coefficients were unavailable or could not be mapped.
  Unknown,
  /// BT.709 matrix coefficients.
  BT709,
  /// BT.601 matrix coefficients.
  BT601,
  /// BT.2020 matrix coefficients.
  BT2020
};

/// @brief Nominal range of the coded luma and chroma samples.
enum class ColorRange {
  /// The nominal range was unavailable or could not be mapped.
  Unknown,
  /// The nominal luma and chroma intervals occupy part of the sample code range.
  Limited,
  /// The nominal sample values use the full code range.
  Full
};

/// @brief Clockwise rotation for displaying the visible image. The planes retain their coded orientation.
enum class VideoRotation {
  /// Display without rotation.
  None,
  /// Rotate 90 degrees clockwise.
  Clockwise90,
  /// Rotate 180 degrees clockwise.
  Clockwise180,
  /// Rotate 270 degrees clockwise.
  Clockwise270
};

/// @brief A crop rectangle in coded luma pixels.
struct VideoRectangle {
  /// Horizontal offset of the first visible column in coded luma pixels.
  std::uint32_t x = 0;
  /// Vertical offset of the first visible row in coded luma pixels.
  std::uint32_t y = 0;
  /// Positive horizontal extent in coded luma pixels. The default zero is unset.
  std::uint32_t width = 0;
  /// Positive vertical extent in coded luma pixels. The default zero is unset.
  std::uint32_t height = 0;
  /// @brief Compares the crop origin and extents.
  /// @return True if both offsets and both extents are equal, otherwise false.
  bool operator==(const VideoRectangle&) const = default;
};

/// @brief Coded image geometry, pixel layout, and presentation metadata.
///
/// Size the planes using the coded dimensions, then use visible, pixel aspect ratio, and rotation for display.
/// Unavailable color metadata remains Unknown so the consumer can choose its own fallback.
struct VideoFormat {
  /// Positive coded width in luma pixels, including columns outside the visible crop.
  std::uint32_t width = 0;
  /// Positive coded height in luma pixels, including rows outside the visible crop.
  std::uint32_t height = 0;
  /// Nonempty crop within the coded image, expressed before presentation rotation.
  VideoRectangle visible;
  /// Plane count, order, and sample layout. Use the frame's strides to address the planes.
  VideoPixelFormat pixel_format = VideoPixelFormat::NV12;
  /// Pixel width and height in the same units. Both must be positive. The default 1:1 describes square pixels.
  std::uint32_t aspect_width = 1;
  /// Positive pixel height in the same units as aspect_width. The default pair 1:1 describes square pixels.
  std::uint32_t aspect_height = 1;
  /// Color primaries for interpreting the coded samples.
  ColorPrimaries primaries = ColorPrimaries::Unknown;
  /// Transfer characteristic for interpreting the coded samples.
  ColorTransfer transfer = ColorTransfer::Unknown;
  /// Matrix coefficients for conversion to RGB.
  ColorMatrix matrix = ColorMatrix::Unknown;
  /// Nominal sample range for luma and chroma.
  ColorRange range = ColorRange::Unknown;
  /// Display rotation to apply after selecting the visible pixels.
  VideoRotation rotation = VideoRotation::None;

  /// @brief Checks the metadata needed to describe a CPU video image.
  /// @return True if coded dimensions and aspect terms are positive, the nonempty crop fits in the coded image,
  /// and all enum fields have recognized values. Unknown color metadata is valid. Use VideoFrame::Validate()
  /// to also check the planes against this description.
  [[nodiscard]] bool Validate() const noexcept;

  /// @brief Compares all geometry, layout, and presentation fields, including the stored aspect ratio terms.
  /// @return True if all stored format fields are equal, otherwise false.
  bool operator==(const VideoFormat&) const = default;
};

/// @brief Retained byte range and strides for one CPU video plane.
///
/// Address the sample at plane coordinates (x, y) with offset y * row_stride + x * pixel_stride from the
/// first byte in data. Chroma coordinates are in the subsampled plane's own sample grid.
struct VideoPlane {
  /// Bytes beginning at the first sample. Keep this reference or a retaining copy while using its AsBytes() span.
  /// The range must cover the entire final addressed sample. Padding after that sample is optional.
  BufferReference data;
  /// Positive byte distance between starts of adjacent rows, including any row padding.
  std::size_t row_stride = 0;
  /// Positive byte distance between adjacent samples in a row. It must cover all bytes of the sample,
  /// including both components in a semiplanar chroma plane.
  std::size_t pixel_stride = 0;
};

/// @brief A retained video image and its timing on the presentation timeline.
///
/// Decoder output owns immutable plane bytes that survive Reset(), Close(), and cleanup completion. A copy,
/// retained plane, or slice holds the frame's output reservation across resets until the last reference is released.
/// For application-provided planes, keep their storage stable and synchronize writes with readers.
/// Platform output retains an immutable image and its reservation through the last frame copy. Keep a frame copy
/// until all GPU reads from the image have completed. VideoEncoder copies CPU pixels and retains platform images
/// on Accepted. The frame must match VideoEncoderConfig::input and use increasing PTS.
/// @see VideoFormat, VideoPlane, BufferReference, VideoEncoder::TrySubmit()
struct VideoFrame {
  /// Creates an empty frame for a caller to fill with CPU planes and metadata.
  VideoFrame() = default;

  /// @brief Creates a CPU frame from caller-provided metadata and plane references.
  /// @param format Coded geometry and presentation metadata for the planes.
  /// @param pts Presentation timestamp on the media timeline.
  /// @param duration Optional display duration.
  /// @param planes Pixel planes whose storage is retained through their buffer references.
  /// Call Validate() before submitting the frame.
  VideoFrame(VideoFormat format, MediaTimestamp pts, std::optional<MediaTimestamp> duration,
      std::vector<VideoPlane> planes);

  /// Complete coded geometry and presentation metadata for these pixels.
  VideoFormat format;
  /// Required presentation timestamp with a positive timescale. Decoder output preserves the associated packet's
  /// exact rational PTS, including negative preroll.
  MediaTimestamp pts;
  /// Optional nonnegative display duration with a positive timescale. For decoder output, packet duration takes
  /// precedence over platform duration. An empty value means duration is unknown. VideoEncoder requires a positive
  /// duration when supplied and restores it on the corresponding encoded packet.
  std::optional<MediaTimestamp> duration;
  /// Y, U, V planes for I420. For NV12, a Y plane followed by one interleaved U/V plane.
  /// Plane extents are based on the coded image. Empty for platform images.
  std::vector<VideoPlane> planes;

  /// Returns the storage kind. Use Validate() to check the frame's format, timing, and image.
  VideoStorage Storage() const noexcept { return platform_storage_ ? VideoStorage::Platform : VideoStorage::Cpu; }

  /// @brief Checks metadata, timing, and storage coverage for the full coded image.
  ///
  /// The luma plane covers the coded dimensions. Each chroma dimension is half its coded dimension rounded up.
  /// Each row must fit its samples and each range must cover the last sample without size or offset overflow.
  /// @return True if format and timing are valid and the CPU planes or platform image fit the coded layout.
  [[nodiscard]] bool Validate() const noexcept;

private:
  friend struct detail::VideoFrameAccess;
  std::shared_ptr<const detail::PlatformVideoStorage> platform_storage_;
};

} // namespace huxerui::media
