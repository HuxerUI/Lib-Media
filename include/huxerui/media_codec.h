#pragma once

/// @file
/// @brief Headless, bounded, asynchronous audio and video encoding and decoding.
///
/// Include <huxerui/media_codec.h> and link HuxerUI::Media. Use these sessions directly from headless code.
/// Decoders accept initialization data and complete compressed access units in decode order, then emit format
/// events and retained frames. The audio encoder accepts contiguous PCM and emits AAC configuration, retained raw
/// access units, and sample accounting. The video encoder accepts CPU YUV frames or supported platform images.
/// It emits codec configuration and retained access units in decode order. A successful drain ends with one EndOfStream event.
/// Platform video frames can be published as HuxerUI textures.

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <huxerui/media_format.h>

namespace huxerui {
class ExternalTexture;
}

namespace huxerui::media {
namespace detail {
template<class Output> class DecoderState;
struct VideoDecoderAccess;
class AudioEncoderState;
class VideoEncoderState;
}

/// @brief Portable categories for configuration, encoding, decoding, and cleanup errors.
///
/// A full input queue reports the successful status SubmitStatus::WouldBlock.
enum class CodecErrorCode {
  /// The codec, profile, CPU layout, or platform capability is unavailable.
  Unsupported,
  /// A configuration, queue limit, or declared stream bound is invalid.
  InvalidArgument,
  /// The operation is not permitted in the current lifecycle phase.
  InvalidState,
  /// An access unit, PCM frame, timestamp, or platform codec value violates the configured contract.
  InvalidData,
  /// A bounded allocation or platform resource request could not be satisfied.
  ResourceExhausted,
  /// The platform API failed without a more specific portable category.
  PlatformFailure,
  /// The session has been logically retired, or the handle has been moved from.
  Closed
};

/// @brief A codec failure with a portable category and optional platform diagnostic information.
struct CodecError {
  /// Stable category for application error handling.
  CodecErrorCode code = CodecErrorCode::PlatformFailure;
  /// Human-readable diagnostic text. Use code for application decisions.
  std::string message;
  /// Platform error number or subsystem identifier when available. An empty string means unavailable.
  std::string platform_code;
};

/// @brief SDK result carrying an owned success value or a CodecError.
///
/// Check Succeeded() before accessing Value() or Error(). Accessing the unavailable branch throws std::logic_error.
/// References from those accessors borrow the result. Copy or move the stored value to keep it independently.
/// @tparam T Success type. Use void for operations that confirm acceptance or completion without a payload.
template<class T> using CodecResult = Result<T, CodecError>;

/// @brief Limits on retained input and output reservations for one session.
///
/// Input capacity counts compressed packets for decoding and PCM or video frames for encoding, including storage still
/// retained by an adapter. It becomes available when the last owner releases the storage. Output capacity is
/// reserved before platform conversion and covers in-flight, reordered, queued, and consumer-held frames or packets.
/// Retaining a plane, slice, or platform frame keeps the entire originating output reservation across Reset().
/// Release retained output to let pending input advance when the output pool is full.
///
/// Decoders require at least two input slots and a positive input byte limit. Audio decoding requires four output
/// slots sized for the largest access unit of the configured codec. Video requires 34 output slots sized for the coded bounds.
/// Charge each video slot by its coded layout. I420 and NV12 use one byte per luma sample and two bytes per
/// chroma position, P010 uses twice that, and Opaque reserves four bytes per coded pixel.
/// Both output limits must accommodate the required slots. Platform codec and adapter scratch allocations are separate.
/// AudioEncoderConfig and VideoEncoderConfig specify their respective input and compressed output minimums.
/// @see AudioDecoderConfig::limits, VideoDecoderConfig::limits, AudioEncoderConfig::limits, VideoEncoderConfig::limits
struct QueueLimits {
  /// Maximum number of input packets or uncompressed frames still retained. Defaults to 48.
  std::size_t input_items = 48;
  /// Maximum total retained input bytes. A larger single packet or frame is rejected. Defaults to 8 MiB.
  std::size_t input_bytes = 8 * 1024 * 1024;
  /// Maximum number of output frame or packet reservations. Defaults to 48, subject to output_bytes.
  std::size_t output_items = 48;
  /// Maximum total bytes reserved for output. Decoders charge the maximum configured frame size per slot.
  /// Audio encoding reserves 64 KiB per pending packet, then reduces the charge to its actual payload size.
  /// Video encoding reserves VideoEncoderConfig::max_packet_bytes per picture and reduces it after encoding.
  /// The default is 192 MiB; individual configurations can supply smaller defaults. Storage is allocated as needed.
  std::size_t output_bytes = 192 * 1024 * 1024;
};

/// @brief Configuration for decoding complete compressed audio access units to CPU PCM.
///
/// The decoder preserves the stream's mono or stereo layout and sample rate. Opus uses a 48 kHz output clock.
/// Choose PCM packing and sample representation in output. Apply container trim and seek preroll to the
/// decoded timeline. Use Probe() to check the selected codec and stream configuration on the current platform.
/// @see AudioDecoder::Create(), AudioDecoder::Probe()
struct AudioDecoderConfig {
  /// Two-byte AudioSpecificConfig for AAC-LC, OpusHead for Opus, or the 34-byte STREAMINFO payload for FLAC.
  /// Leave empty for MPEG Layer III, whose complete frames carry their own configuration.
  Bytes initialization_data;
  /// Requested PCM format. Set the sample rate and channel layout to match the compressed stream.
  /// S16 and F32 are available with either interleaved or planar packing, subject to backend support.
  AudioFormat output;
  /// Retained input and output bounds. See QueueLimits for the audio minimum and byte accounting.
  QueueLimits limits;
  /// Optional coalesced signal for readiness, output, returned queue capacity, or an asynchronous error.
  /// Schedule application work to drain TryRead() and retry pending input until no further progress is possible.
  /// This callback may call Close(). It must return promptly without throwing or waiting for decoder work.
  /// Use owned or weak captures that remain safe if initialization runs before Create() returns.
  std::function<void()> wakeup;
  /// Optional final callback delivered once when teardown completes after Close(), destruction, or fatal failure.
  /// It follows all admitted wakeups and, on desktop and mobile platforms, the exit of the session's worker.
  /// Success confirms platform resources have been released or safely detached at the codec API boundary.
  /// The driver or browser can reclaim its own storage later. Failure reports a cleanup error independently of
  /// any decoding error. Platform work that cannot safely be retired leaves it pending.
  /// Retained CPU frames remain readable and do not delay completion. The callback must return promptly and not throw.
  std::function<void(CodecResult<void>)> cleanup_completed;
  /// Coding of the configuration and access units. Use Aac, Opus, Flac, or Mp3.
  CodecId codec = CodecId::Aac;
};

/// @brief Configuration for decoding video packets to retained frames.
///
/// AVC supports Baseline, Main, and High profiles with at most 16 reference pictures and continuous frame
/// numbering. HEVC supports single-layer Main and Main10 4:2:0. VP8 and VP9 profile 0 produce eight-bit 4:2:0.
/// AV1 supports single-spatial-layer Main, eight-bit SDR 4:2:0, with CPU output.
/// Probe the concrete configuration for platform support, storage, and pixel layout. A fresh or reset session
/// requires a key-marked AVC/HEVC IDR or VP8, VP9, or AV1 key frame.
/// @see VideoDecoder::Create(), VideoDecoder::Probe()
struct VideoDecoderConfig {
  /// Decoder configuration of at most 64 KiB. Supply avcC with SPS/PPS for AVC or hvcC with complete VPS/SPS/PPS for HEVC.
  /// AVC and HEVC pictures use the configuration's NAL length field size of 1, 2, or 4 bytes.
  /// VP8 leaves this empty. VP9 profile 0 accepts an empty configuration or Matroska/WebM codec feature metadata.
  /// For AV1, supply the AV1CodecConfigurationRecord from av1C or Matroska/WebM CodecPrivate.
  Bytes initialization_data;
  /// Maximum coded luma width in pixels, in [1, 8192]. The backend can impose smaller limits.
  /// Together with height, configures the codec and sizes output reservations. Use a new session for larger bounds.
  std::uint32_t width = 0;
  /// Maximum coded luma height in pixels, in [1, 8192], subject to the platform limit described for width.
  std::uint32_t height = 0;
  /// Requested pixel layout. Use I420 or NV12 for eight-bit CPU output and P010 for ten-bit CPU output.
  /// SDR output with platform storage uses NV12 on macOS, iOS, and Windows, and Opaque on Android and Web.
  VideoPixelFormat output = VideoPixelFormat::NV12;
  /// Retained input and output bounds. See QueueLimits for the video minimum and byte accounting.
  QueueLimits limits;
  /// Optional readiness callback with the delivery, capture, and execution contract of AudioDecoderConfig::wakeup.
  std::function<void()> wakeup;
  /// Optional teardown callback with the resource and lifetime contract of AudioDecoderConfig::cleanup_completed.
  /// Returned platform frames may retain their retired image pool independently of this callback.
  std::function<void(CodecResult<void>)> cleanup_completed;
  /// Coding of the supplied configuration and packets. Supports H264, Hevc, Vp8, Vp9, and Av1.
  /// QueryCapabilities() describes H.264. Use Probe() to check other concrete configurations.
  CodecId codec = CodecId::H264;
  /// Selects the returned frame storage. Platform storage requires a compatible codec on macOS, iOS, Android,
  /// Windows, or Web. On Web, create, use, and release a platform decoder and its frames on the browser main thread.
  VideoStorage output_storage = VideoStorage::Cpu;
};

/// @brief H.264 profiles described by the video codec APIs.
enum class H264Profile {
  /// Baseline or Constrained Baseline, without B pictures.
  Baseline,
  /// Main profile with optional B pictures.
  Main,
  /// Eight-bit 4:2:0 High profile with optional B pictures.
  High
};

/// @brief Knowledge about an individual codec capability at query time.
///
/// Supported means the option is admitted by the adapter or advertised by a matching platform component for
/// at least one configuration. It does not imply that independently listed options can be combined arbitrarily.
/// Unknown must not be treated as Unsupported. Probe the complete configuration before creating a session.
enum class CodecSupport {
  /// The platform did not expose enough information to decide.
  Unknown,
  /// The library rejects the option or a required platform facility is definitely unavailable.
  Unsupported,
  /// The option is admitted or advertised, subject to configuration and resource checks.
  Supported
};

/// @brief One library-supported candidate value and the knowledge available about its platform support.
/// @tparam T Audio/video representation, sample rate, channel layout, or H.264 profile value.
template<class T> struct CodecCapability {
  /// Candidate value. Query results include the library's candidates even when platform information is unknown.
  T value{};
  /// Individual support information; it is not a promise about combinations with other candidates.
  CodecSupport support = CodecSupport::Unknown;
};

/// @brief Inclusive unsigned bounds and a reported increment for a codec property.
///
/// Values are bounding information rather than a complete list of valid configurations. A valid value is within
/// [minimum, maximum] and follows minimum + n * step, but other codec constraints can still reject it.
struct CodecUIntRange {
  /// Smallest reported value.
  std::uint32_t minimum = 0;
  /// Largest reported value, at least minimum.
  std::uint32_t maximum = 0;
  /// Positive reported increment. One means no narrower increment is known.
  std::uint32_t step = 1;
};

/// @brief Audio property bounds reported by library validation or a platform component.
///
/// An empty field means unknown or unreported. In particular, zero is never substituted for an unknown maximum.
/// Sample-rate bounds do not replace the discrete sample_rates candidates or the AAC initialization contract.
struct CodecAudioLimits {
  /// Sample-rate bounds in samples per second per channel, when known.
  std::optional<CodecUIntRange> sample_rate;
  /// Channel-count bounds, when known. The library currently admits mono and stereo only.
  std::optional<CodecUIntRange> channels;
  /// Average bitrate bounds in bits per second, when known.
  std::optional<CodecUIntRange> bitrate;
};

/// @brief Video property bounds reported by library validation or a platform component.
///
/// Bounds can depend on profile, dimensions, frame rate, and other settings. They do not describe a Cartesian
/// product of supported configurations. An empty field is unknown; use Probe() to validate a selected combination.
struct CodecVideoLimits {
  /// Coded luma width bounds, when known.
  std::optional<CodecUIntRange> width;
  /// Coded luma height bounds, when known.
  std::optional<CodecUIntRange> height;
  /// Required width multiple, independently of the reported width interval, when known.
  std::optional<std::uint32_t> width_alignment;
  /// Required height multiple, independently of the reported height interval, when known.
  std::optional<std::uint32_t> height_alignment;
  /// Maximum reported frames per second, when known; positive input frame rates remain required for encoding.
  std::optional<double> maximum_frame_rate;
  /// Average bitrate bounds in bits per second, when known.
  std::optional<CodecUIntRange> bitrate;
};

/// @brief Discovery snapshot for an audio codec on the current adapter.
///
/// Availability describes discovery of a suitable component, not successful decoding or a resource reservation.
/// Library limits and platform limits are separate: an absent platform bound must not be replaced by a library
/// maximum and presented as a device limit. These snapshots own their values and can be retained independently.
/// @see AudioDecoder::QueryCapabilities(), AudioDecoder::Probe(), CodecSupport
struct AudioDecoderCapabilities {
  /// Whether the adapter discovered a suitable component. Unknown allows a later concrete Probe().
  CodecSupport availability = CodecSupport::Unknown;
  /// Whether the current adapter can use hardware acceleration for some configuration, when reliably known.
  /// This does not identify the implementation selected by a later Create().
  CodecSupport hardware_acceleration = CodecSupport::Unknown;
  /// Optional diagnostic explaining unavailable or incomplete discovery; not a stable machine-readable code.
  std::string diagnostic;
  /// Candidate PCM sample representations at the library output boundary.
  std::vector<CodecCapability<AudioSampleFormat>> sample_formats;
  /// Candidate PCM packing modes at the library output boundary.
  std::vector<CodecCapability<AudioPacking>> packings;
  /// Candidate channel layouts; decoding preserves the stream's channel layout.
  std::vector<CodecCapability<AudioChannelLayout>> channel_layouts;
  /// Candidate PCM sample rates; Opus uses 48 kHz.
  std::vector<CodecCapability<std::uint32_t>> sample_rates;
  /// Fixed bounds imposed by this library.
  CodecAudioLimits library_limits;
  /// Bounds actually reported by the platform for matching components, where available.
  CodecAudioLimits platform_limits;
};

/// @brief Discovery snapshot for the current AAC-LC encoder adapter.
///
/// Support has the evidence and combination semantics of AudioDecoderCapabilities. Query success can report
/// Unsupported availability; a failed result instead means the discovery operation itself failed.
/// @see AudioEncoder::QueryCapabilities(), AudioEncoder::Probe(), CodecSupport
struct AudioEncoderCapabilities {
  /// Discovery of a suitable AAC-LC encoder component.
  CodecSupport availability = CodecSupport::Unknown;
  /// Hardware acceleration availability, when reliably known; not a later session's implementation identity.
  CodecSupport hardware_acceleration = CodecSupport::Unknown;
  /// Optional diagnostic about unavailable or incomplete discovery.
  std::string diagnostic;
  /// Candidate input sample representations.
  std::vector<CodecCapability<AudioSampleFormat>> sample_formats;
  /// Candidate input packing modes.
  std::vector<CodecCapability<AudioPacking>> packings;
  /// Candidate input channel layouts.
  std::vector<CodecCapability<AudioChannelLayout>> channel_layouts;
  /// Candidate input sample rates.
  std::vector<CodecCapability<std::uint32_t>> sample_rates;
  /// Fixed bounds imposed by this library.
  CodecAudioLimits library_limits;
  /// Bounds actually reported by the platform, where available.
  CodecAudioLimits platform_limits;
};

/// @brief Discovery snapshot for the current progressive eight-bit 4:2:0 H.264 decoder adapter.
/// @see VideoDecoder::QueryCapabilities(), VideoDecoder::Probe(), AudioDecoderCapabilities
struct VideoDecoderCapabilities {
  /// Discovery of a suitable H.264 decoder component.
  CodecSupport availability = CodecSupport::Unknown;
  /// Hardware acceleration availability, when reliably known; not a later session's implementation identity.
  CodecSupport hardware_acceleration = CodecSupport::Unknown;
  /// Optional diagnostic about unavailable or incomplete discovery.
  std::string diagnostic;
  /// Candidate CPU output layouts. Actual output negotiation remains part of decoding.
  std::vector<CodecCapability<VideoPixelFormat>> pixel_formats;
  /// Candidate stream profiles, subject to coded bounds and valid initialization data.
  std::vector<CodecCapability<H264Profile>> profiles;
  /// Fixed coded-picture bounds imposed by this library.
  CodecVideoLimits library_limits;
  /// Bounds actually reported by the platform, where available.
  CodecVideoLimits platform_limits;
};

/// @brief H.264 encoder profile and independently described B-picture control policies.
///
/// A supported policy does not require the encoder to produce B pictures. Profile and all selected controls must
/// still pass a concrete Probe(). The library's maximum numeric B bound is 16; platform limits can be smaller.
struct H264EncoderProfileCapabilities {
  /// Requested stream profile.
  H264Profile profile = H264Profile::Baseline;
  /// Knowledge about the profile independently of a particular size/rate/control combination.
  CodecSupport support = CodecSupport::Unknown;
  /// Ability to request max_b_frames = 0 for this profile.
  CodecSupport no_b_frames = CodecSupport::Unknown;
  /// Ability to request max_b_frames = std::nullopt for this profile.
  CodecSupport automatic_b_frames = CodecSupport::Unknown;
  /// Ability to request a positive numeric consecutive B-picture upper bound for this profile.
  CodecSupport bounded_b_frames = CodecSupport::Unknown;
  /// Known ceiling usable by this adapter, including its policy limits. Zero means no positive bound is usable.
  /// An empty value means the ceiling is unknown, independently of bounded_b_frames.
  std::optional<std::uint32_t> maximum_b_frames;
};

/// @brief Discovery snapshot for the current CPU H.264 encoder adapter.
/// @see VideoEncoder::QueryCapabilities(), VideoEncoder::Probe(), AudioDecoderCapabilities
struct VideoEncoderCapabilities {
  /// Discovery of a suitable H.264 encoder component.
  CodecSupport availability = CodecSupport::Unknown;
  /// Hardware acceleration availability, when reliably known; not a later session's implementation identity.
  CodecSupport hardware_acceleration = CodecSupport::Unknown;
  /// Optional diagnostic about unavailable or incomplete discovery.
  std::string diagnostic;
  /// Candidate CPU input layouts. Layout conversion does not change the configured color values.
  std::vector<CodecCapability<VideoPixelFormat>> pixel_formats;
  /// Candidate profiles and their separate B-picture policies.
  std::vector<H264EncoderProfileCapabilities> profiles;
  /// Fixed input-picture, rate, and bitrate bounds imposed by this library.
  CodecVideoLimits library_limits;
  /// Bounds actually reported by the platform, where available.
  CodecVideoLimits platform_limits;
};

/// @brief Fixed configuration for encoding video frames to H.264 or HEVC access units.
///
/// Supply even dimensions in [2, 8192], a full visible rectangle, square pixels, no rotation, and SDR color facts.
/// Every submitted frame must match input. CPU plane strides may differ. CPU H.264 and HEVC Main take I420 or NV12.
/// CPU HEVC Main10 takes P010.
/// Probe the concrete codec, storage, layout, dimensions, rate, and B-picture policy.
/// @see VideoEncoder::Create(), VideoEncoder::Probe(), VideoEncodeFormat
struct VideoEncoderConfig {
  /// Required geometry, pixel layout, and SDR color metadata. The default unset VideoFormat is invalid.
  VideoFormat input;
  /// Positive numerator of the expected frame rate. This guides rate control; input PTS remains authoritative.
  std::uint32_t frame_rate_numerator = 30;
  /// Positive denominator; 30000/1001 represents approximately 29.97 frames/second. The ratio cannot exceed 240 fps.
  std::uint32_t frame_rate_denominator = 1;
  /// Positive target average bits per second, at most INT32_MAX. This is not a per-packet size guarantee.
  std::uint32_t bitrate = 4'000'000;
  /// Requested H.264 profile. HEVC derives its profile from input.pixel_format.
  H264Profile profile = H264Profile::High;
  /// Target distance in input pictures between random-access pictures, in [1, INT32_MAX]. Platforms may insert them earlier.
  /// This is a rate-control request, not an exact frame boundary guarantee.
  std::uint32_t key_frame_interval = 60;
  /// Zero forbids B pictures; a positive value up to 16 bounds consecutive B pictures without requiring their use.
  /// An empty value permits platform selection within the bounded processing window. Baseline rejects positive values.
  /// Platforms that cannot enforce a requested numeric bound return Unsupported.
  std::optional<std::uint32_t> max_b_frames = 0;
  /// Hard bound in [1, INT32_MAX] on one access unit after normalization to four-byte NAL lengths. Defaults to 4 MiB.
  /// Output storage is reserved before submission, then reduced to the actual packet size. Oversize output fails.
  std::size_t max_packet_bytes = 4 * 1024 * 1024;
  /// Maximum copied input and retained output. Input must fit one packed frame. Output needs at least two full
  /// packet reservations, or max_b_frames + 2 when an explicit positive bound is requested. Platform delay may
  /// require a larger window. Defaults: four inputs/64 MiB and eight outputs/32 MiB; allocations are demand-driven.
  /// Returned slices retain their entire originating packet credit across resets. Platform storage is additional.
  QueueLimits limits{4, 64 * 1024 * 1024, 8, 32 * 1024 * 1024};
  /// Optional coalesced signal for readiness, output, returned capacity, or an asynchronous error.
  /// Schedule application work to drain TryRead() and retry pending input until no further progress is possible.
  /// This callback runs outside library locks and may call Close(). Return promptly without throwing or waiting.
  /// Use owned or weak captures that remain safe if initialization runs before Create() returns.
  std::function<void()> wakeup;
  /// Optional final result after platform release, admitted wakeup return, and desktop/mobile worker exit.
  /// On Web it runs on the owning event loop after retirement. Held packet bytes do not delay it. A cleanup failure
  /// is separate from an encoding failure; unsafe outstanding platform work leaves completion pending.
  /// Return promptly without throwing or waiting. Use owned or weak captures that survive retirement.
  std::function<void(CodecResult<void>)> cleanup_completed;
  /// CodecId::H264 or CodecId::Hevc. Defaults to H.264.
  CodecId codec = CodecId::H264;
  /// Selects submitted frame storage. Platform SDR input takes NV12 on macOS, iOS, and Windows, and Opaque on Android
  /// and Web. Create, use, and release Web platform encoders and their input frames on the browser main thread.
  /// Check the complete configuration with Probe().
  VideoStorage input_storage = VideoStorage::Cpu;
};

/// @brief Video configuration emitted before its dependent access units.
///
/// Coded bounds may include padding beyond visible. Use these bounds when configuring VideoDecoder.
/// The configuration record carries the stream profile, bit depth, and any color signaling.
/// H.264 encoding validates any explicitly requested color signaling against the emitted configuration.
/// Initialization data carries the platform-selected level; there is no separate level request in this API.
/// @see VideoEncodeOutput, VideoDecoderConfig
struct VideoEncodeFormat {
  /// Actual coded luma width, including any padding outside the visible picture.
  std::uint32_t width = 0;
  /// Actual coded luma height, including any padding outside the visible picture.
  std::uint32_t height = 0;
  /// Visible input picture within the coded bounds.
  VideoRectangle visible;
  /// AVCDecoderConfigurationRecord or HEVCDecoderConfigurationRecord of at most 64 KiB.
  /// Parameter sets are out of band and access units use four-byte NAL lengths.
  Bytes initialization_data;
  /// Codec used by the configuration record and following access units.
  CodecId codec = CodecId::H264;
  /// @brief Compares codec, coded bounds, visible rectangle, and initialization data.
  /// @return True when all stored fields match.
  bool operator==(const VideoEncodeFormat&) const = default;
};

/// @brief Configuration for encoding interleaved S16 PCM to raw AAC-LC access units.
///
/// Supports mono or stereo at 44100 or 48000 Hz. Input is encoded at the configured rate and channel layout without
/// resampling or channel mixing. The output contains one raw AAC-LC access unit per packet, with initialization
/// data delivered separately. Probe the concrete configuration for platform codec and bitrate support.
/// @see AudioEncoder::Create(), AudioEncoder::Probe(), AudioEncodeFormat
struct AudioEncoderConfig {
  /// Required PCM format. Set a supported sample rate, S16 representation, interleaved packing, and mono/stereo
  /// layout. Every submitted frame must match all four fields. The default AudioFormat is not a valid encoder input.
  AudioFormat input;
  /// Positive target bits per second, up to INT32_MAX. Defaults to 128000. An unsupported platform target is rejected;
  /// the encoder does not silently replace it. This is a bitrate target rather than an exact size for each packet.
  std::uint32_t bitrate = 128000;
  /// Bounds on copied PCM and compressed output. Input needs one slot and one sample frame of bytes.
  /// Output needs at least 16 slots and 1 MiB. Each pending packet reserves 64 KiB before platform work,
  /// including encoder delay and drain output; returned packets retain only their actual byte charge.
  /// A separate PCM assembly buffer holds at most 1024 samples per channel. Platform scratch is bounded separately.
  /// Defaults are 48 input slots, 256 KiB copied PCM, 48 output slots, and 3 MiB compressed output reservations.
  /// Held output shares the same pool across resets. See QueueLimits for retained-storage accounting.
  QueueLimits limits{48, 256 * 1024, 48, 3 * 1024 * 1024};
  /// Optional coalesced signal for readiness, output, returned queue capacity, or an asynchronous error.
  /// Schedule application work to drain TryRead() and retry pending PCM until no further progress is possible.
  /// This callback may call Close(). It must return promptly without throwing or waiting for encoder work.
  /// Use owned or weak captures that remain safe if initialization runs before Create() returns.
  std::function<void()> wakeup;
  /// Optional final callback delivered once when teardown completes after Close(), destruction, or fatal failure.
  /// It follows all admitted wakeups and, on desktop and mobile platforms, the exit of the session's worker.
  /// Success confirms platform resources have been released or safely detached at the codec API boundary.
  /// The driver or browser can reclaim its own storage later. Failure reports a cleanup error independently of
  /// any encoding error. Platform work that cannot safely be retired leaves it pending.
  /// Retained encoded packets remain readable and do not delay completion. Return promptly without throwing.
  std::function<void(CodecResult<void>)> cleanup_completed;
};

/// @brief Stream description preceding the first raw AAC packet, including after Reset().
///
/// The configuration remains fixed for one stream. Supply initialization_data to AudioDecoderConfig when decoding
/// the emitted packets. Container framing such as ADTS or MP4 is supplied by the application, not the encoder.
/// @see AudioEncodeOutput, AudioEncoderConfig, AudioDecoderConfig
struct AudioEncodeFormat {
  /// Encoded sample rate, equal to the configured PCM rate.
  std::uint32_t sample_rate = 0;
  /// Encoded speaker layout, equal to the configured PCM layout.
  AudioChannelLayout channel_layout = AudioChannelLayout::Stereo;
  /// Two-byte MPEG-4 AudioSpecificConfig for AAC-LC with 1024 samples per access unit.
  Bytes initialization_data;

  /// @brief Compares the sample rate, channel layout, and initialization bytes.
  /// @return True if all three fields are equal, otherwise false.
  bool operator==(const AudioEncodeFormat&) const = default;
};

/// @brief Sample accounting emitted once after all packets of a successful drain.
///
/// Counts are samples per channel, not interleaved channel values. The encoder emits complete access units and
/// reports any known leading delay and trailing padding separately. Unknown trim values remain absent; zero means
/// a known absence of trimming. An empty stream has no input_start and reports zero for every count and trim value.
/// Reset discards accounting for the retired stream. A failed or closed stream does not emit a successful summary.
/// @see AudioEncoder::Finish(), AudioEncodeOutput
struct AudioEncodeSummary {
  /// First accepted PTS normalized to the sample grid; absent for an empty stream.
  std::optional<MediaTimestamp> input_start;
  /// Total accepted samples per channel, excluding any library tail fill.
  std::uint64_t input_samples = 0;
  /// Total samples represented by the emitted access units, including delay and tail padding.
  std::uint64_t encoded_samples = 0;
  /// Leading encoder delay in samples per channel, when reported reliably by the platform encoder.
  /// Unknown is distinct from zero. No fixed delay or timing-based estimate is substituted.
  std::optional<std::uint64_t> priming_samples;
  /// Trailing padding in samples per channel, including library tail fill, when platform trim information permits
  /// exact accounting. The encoder can derive this from a known leading delay and the final sample totals.
  /// When both trim values are known, encoded_samples - priming_samples - padding_samples == input_samples.
  std::optional<std::uint64_t> padding_samples;
};

/// @brief End of a successfully drained stream.
///
/// A codec session emits this once after Finish() and all delayed output. An audio encoder emits its sample summary
/// immediately before this event. The session remains open for Reset() or Close().
/// @see AudioDecoder::Finish(), VideoDecoder::Finish(), AudioEncoder::Finish(), VideoEncoder::Finish()
struct EndOfStream {};

/// @brief One audio output event from TryRead().
///
/// AudioFormat precedes the first frame using a new PCM format, including the first frame after Reset().
/// Each AudioFrame also carries its format. EndOfStream follows the last frame of a successful drain.
using AudioDecodeOutput = std::variant<AudioFormat, AudioFrame, EndOfStream>;

/// @brief One video output event from TryRead().
///
/// VideoFormat precedes the first frame using a new layout or presentation description, including after Reset().
/// VideoFrame events follow presentation order. Each carries its format. EndOfStream follows a successful drain.
using VideoDecodeOutput = std::variant<VideoFormat, VideoFrame, EndOfStream>;

/// @brief One audio encoding event from TryRead().
///
/// AudioEncodeFormat precedes its dependent packets, including after Reset(). Each EncodedPacket contains one raw
/// AAC access unit with a duration of 1024 samples per channel. AudioEncodeSummary follows the last packet, then
/// EndOfStream appears once. Empty input produces a zero-count summary and EndOfStream without packets; a format
/// event can also be available if the platform encoder reports its configuration before receiving PCM.
/// @see AudioEncoder::TryRead(), AudioEncodeSummary
using AudioEncodeOutput = std::variant<AudioEncodeFormat, EncodedPacket, AudioEncodeSummary, EndOfStream>;

/// @brief A video configuration, retained coded access unit, or successful end of the stream.
///
/// Format precedes dependent packets, including after Reset(). Each packet represents one input picture and uses
/// four-byte big-endian NAL lengths. Packets are emitted in decode order; PTS may move backwards with B pictures.
/// EndOfStream appears once after all accepted pictures. A failed or closed stream emits no successful end.
/// @see VideoEncoder::TryRead(), VideoEncodeFormat
using VideoEncodeOutput = std::variant<VideoEncodeFormat, EncodedPacket, EndOfStream>;

/// @brief Move-only AAC-LC, Opus, FLAC, or MP3 decoder with bounded queues and independently retained PCM output.
///
/// The returned handle owns the session. On desktop and mobile platforms, queue and control calls may run concurrently on a live
/// handle. On Web, use the JavaScript event loop that created the session. Move or destroy a handle only after its
/// concurrent callers have stopped. Codec operations can copy bytes and acquire locks, so schedule them outside hard
/// real-time callbacks.
///
/// Callbacks for one session are serialized and dispatched outside library locks. Each desktop or mobile session runs codec
/// work and wakeups on its own worker. Cleanup completion is dispatched after that worker exits. These callbacks
/// may race the return of Create() or a control call. On Web both callbacks run on the owning event loop.
///
/// An asynchronous initialization or decoding failure starts teardown. TryRead() and TrySubmit() report that
/// failure until Close() changes the session to Closed. The optional cleanup callback reports teardown separately.
/// @see AudioDecoderConfig, VideoDecoder
class AudioDecoder final {
public:
  /// @brief Validates the configuration and schedules platform initialization.
  /// @param config AAC setup data, PCM format, queue limits, and optional callbacks, copied for the session.
  /// @return An owning decoder handle on success. Initialization may still be pending, in which case TrySubmit()
  /// returns WouldBlock. A synchronous Failure returns a CodecError and schedules no callbacks.
  /// @see Probe(), TryRead(), AudioDecoderConfig::wakeup
  static CodecResult<AudioDecoder> Create(const AudioDecoderConfig& config);

  /// @brief Tests the configuration asynchronously by initializing and releasing an independent platform codec.
  ///
  /// The probe uses its own completion. A later Create() acquires its own resources and validates input during decode.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On desktop and mobile platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
  /// It must return promptly without throwing or waiting for decoder work.
  static void Probe(const AudioDecoderConfig& config, std::function<void(CodecResult<void>)> completion);

  /// @brief Asynchronously discovers the current adapter's AAC-LC decoding capabilities without decoding input.
  /// @param completion Optional callback invoked once with an owned snapshot or a query error. It runs after the
  /// independent native worker exits, or asynchronously on the creating Web event loop. Return promptly without
  /// throwing or waiting. No Runtime, window, Install(), or live decoder is required.
  /// A successful query may report Unsupported or Unknown availability. Unknown fields remain unknown; supported
  /// options and bounds do not guarantee a combined configuration or reserve resources for Create().
  /// @throws std::bad_alloc If query-state allocation fails before scheduling; no callback is scheduled.
  /// @throws std::system_error If a native worker cannot start; no callback is scheduled.
  /// @see AudioDecoderCapabilities, Probe(), CodecSupport
  static void QueryCapabilities(std::function<void(CodecResult<AudioDecoderCapabilities>)> completion);

  /// @brief Checks for a decoder using a representative configuration of the requested audio codec.
  /// @param codec Aac, Opus, Flac, or Mp3. Aac uses the platform discovery query above.
  /// @param completion Optional callback with the delivery and lifetime contract of the AAC query above.
  /// A rejected reference configuration leaves availability unknown. Use Probe() for the actual stream and PCM format.
  static void QueryCapabilities(CodecId codec, std::function<void(CodecResult<AudioDecoderCapabilities>)> completion);

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  AudioDecoder(AudioDecoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle of a transfer then behaves as closed.
  AudioDecoder& operator=(AudioDecoder&&) noexcept;
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~AudioDecoder();

  /// @brief Queues one complete audio access unit without waiting for platform decoding.
  /// @param packet Complete access unit with a nonempty payload and valid timing. Submit packets in decode order
  /// with nondecreasing PTS and keep the bytes unchanged until the call returns.
  /// @return Accepted after copying the packet. WouldBlock leaves it unconsumed while startup or reset is pending
  /// or input capacity is exhausted. Failure reports packet rejection, InvalidState while draining or ended,
  /// Closed after retirement or on a moved-from handle, or the recorded operational error after fatal failure.
  /// A packet rejected by synchronous validation leaves the decoder available for valid input.
  /// @see EncodedPacket, SubmitStatus, TryRead()
  CodecResult<SubmitStatus> TrySubmit(const EncodedPacket& packet);

  /// @brief Removes the next output event without waiting for platform decoding.
  ///
  /// A format event precedes its dependent frames. Retain a frame or plane by copying or moving its value.
  /// @return Failure with the recorded operational error or Closed. On success, std::nullopt means no event is
  /// available. An engaged value carries one AudioFormat, AudioFrame, or EndOfStream. EndOfStream is consumed
  /// once per successful drain. Later reads return an empty optional until a new stream starts or the session closes.
  ///
  /// This consumer uses the format stored on each frame. Its success value indicates whether it read EndOfStream.
  /// @code{.cpp}
  /// using namespace huxerui::media;
  /// CodecResult<bool> DrainAudio(AudioDecoder& decoder,
  ///     const std::function<void(const AudioFrame&)>& consume) {
  ///   for (;;) {
  ///     auto result = decoder.TryRead();
  ///     if (!result.Succeeded()) return CodecResult<bool>::Failure(result.Error());
  ///     if (!result.Value()) return CodecResult<bool>::Success(false);
  ///     const auto& output = *result.Value();
  ///     if (const auto* frame = std::get_if<AudioFrame>(&output)) consume(*frame);
  ///     else if (std::holds_alternative<EndOfStream>(output)) return CodecResult<bool>::Success(true);
  ///   }
  /// }
  /// @endcode
  CodecResult<std::optional<AudioDecodeOutput>> TryRead();

  /// @brief Seals input and starts an asynchronous drain of accepted packets.
  ///
  /// Read remaining frames and then EndOfStream through TryRead(). Held output can delay the drain until capacity
  /// is released. Further submissions return InvalidState while draining or ended. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset or after a fatal error. Closed after retirement or on a moved-from handle.
  CodecResult<void> Finish();

  /// @brief Discards pending input and output and schedules a fresh stream with the same configuration.
  ///
  /// The discard takes effect on acceptance, including during a drain. Frames already returned or acquired by a
  /// concurrent read before the reset remain readable and hold their reservations. TrySubmit() returns WouldBlock
  /// until platform recreation finishes. Read any recreation error with TryRead(). Use Create() for a new configuration.
  /// @return Success when the reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Retirement ends further input, reads, and wakeup admission. A wakeup
  /// admitted before retirement may still run after return. Use cleanup_completed to observe platform release and
  /// callback quiescence before retiring state used by wakeup. Allow the final callback to return and fence all
  /// codec work before unloading the module or terminating its worker.
  /// @see AudioDecoderConfig::cleanup_completed
  void Close() noexcept;
private:
  explicit AudioDecoder(std::shared_ptr<detail::DecoderState<AudioDecodeOutput>> state);
  std::shared_ptr<detail::DecoderState<AudioDecodeOutput>> state_;
};

/// @brief Move-only video decoder with bounded queues and retained video frames.
///
/// Submit complete picture packets in decode order, starting with a key-marked random-access packet.
/// Output arrives in presentation order. Backends that need a 16-frame reordering window can hold frames until more input
/// arrives or Finish() drains it. Android MediaCodec publishes surface output in presentation order.
/// See AudioDecoder for the shared execution, callback, and handle ownership contract.
/// @see VideoDecoderConfig, VideoFrame, AudioDecoder
class VideoDecoder final {
public:
  /// @brief Validates the video configuration and schedules platform initialization.
  /// @param config Codec identifier, initialization data, coded bounds, layout, limits, and callbacks, copied for the session.
  /// @return An owning decoder handle on success. TrySubmit() can return WouldBlock while initialization is pending.
  /// A synchronous Failure returns a CodecError and schedules no callbacks.
  /// @see Probe(), TryRead(), VideoDecoderConfig::wakeup
  static CodecResult<VideoDecoder> Create(const VideoDecoderConfig& config);

  /// @brief Tests platform initialization and release for the supplied video configuration asynchronously.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On desktop and mobile platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
  /// It must return promptly without throwing or waiting for decoder work.
  /// @see AudioDecoder::Probe()
  static void Probe(const VideoDecoderConfig& config, std::function<void(CodecResult<void>)> completion);

  /// @brief Asynchronously discovers the current adapter's H.264 decoding capabilities without decoding input.
  /// @param completion Optional owned-snapshot or query-error callback with the execution, lifetime, and exception
  /// contract of AudioDecoder::QueryCapabilities(). Input still requires valid avcC and complete access units.
  /// @see VideoDecoderCapabilities, Probe(), AudioDecoder::QueryCapabilities()
  static void QueryCapabilities(std::function<void(CodecResult<VideoDecoderCapabilities>)> completion);

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  VideoDecoder(VideoDecoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle of a transfer then behaves as closed.
  VideoDecoder& operator=(VideoDecoder&&) noexcept;
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~VideoDecoder();

  /// @brief Queues one complete picture packet without waiting for platform decoding.
  /// @param packet Packet in decode order with valid timing. AVC and HEVC NAL lengths match initialization_data.
  /// VP8 and VP9 packets may carry reference-only frames. VP9 superframes can contain up to one visible picture.
  /// AV1 uses one temporal unit per packet with at most one visible picture. An AV1 key packet carries an
  /// in-band sequence header before its first frame, a displayed key frame.
  /// The first packet after creation or reset must be a random-access packet and set key_frame.
  /// Keep bytes unchanged until return.
  /// @return Accepted after copying the packet. WouldBlock leaves it unconsumed while startup or reset is pending
  /// or input capacity is exhausted. Failure reports packet rejection, InvalidState while draining or ended,
  /// Closed after retirement or on a moved-from handle, or the recorded operational error after fatal failure.
  /// @see EncodedPacket, SubmitStatus, AudioDecoder::TrySubmit()
  CodecResult<SubmitStatus> TrySubmit(const EncodedPacket& packet);

  /// @brief Removes the next output event without waiting for platform decoding or reordering.
  /// @return Failure with the recorded operational error or Closed. On success, std::nullopt means no event is
  /// available. An engaged value carries one VideoFormat, VideoFrame, or EndOfStream. Format events precede
  /// their dependent frames. EndOfStream is delivered once per successful drain, then reads are empty.
  /// @see VideoDecodeOutput, VideoFrame, AudioDecoder::TryRead()
  CodecResult<std::optional<VideoDecodeOutput>> TryRead();

  /// @brief Seals input and asynchronously drains platform and reordered frames before EndOfStream.
  ///
  /// Continue reading and releasing frames to allow the drain to complete. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset or after a fatal error. Closed after retirement or on a moved-from handle.
  /// @see AudioDecoder::Finish()
  CodecResult<void> Finish();

  /// @brief Discards pending input and output and schedules a fresh stream with the same video configuration.
  ///
  /// Previously returned frames retain storage and reservations. TrySubmit() returns WouldBlock until the new
  /// stream is ready. Start it with a key-marked random-access packet as described in TrySubmit().
  /// Use Create() to change coded bounds or initialization data.
  /// @return Success when the reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  /// @see AudioDecoder::Reset()
  CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Earlier admitted callbacks may still run after return. Observe platform
  /// release and callback quiescence through cleanup_completed. Previously returned CPU frames remain readable.
  /// @see VideoDecoderConfig::cleanup_completed, AudioDecoder::Close()
  void Close() noexcept;
private:
  explicit VideoDecoder(std::shared_ptr<detail::DecoderState<VideoDecodeOutput>> state);
  std::shared_ptr<detail::DecoderState<VideoDecodeOutput>> state_;
  friend struct detail::VideoDecoderAccess;
};

/// @brief Move-only AAC-LC encoder with bounded queues and independently retained compressed output.
///
/// Submit contiguous interleaved S16 PCM and drain raw access units through TryRead(). The returned handle owns
/// the session. No Runtime, window, capture source, or playback device is required.
///
/// On desktop and mobile platforms, queue and control calls may run concurrently on a live handle. On Web, use the JavaScript
/// event loop that created the session. Move or destroy a handle only after its concurrent callers have stopped.
/// Codec operations can copy bytes and acquire locks, so schedule them outside hard real-time callbacks.
///
/// Callbacks for one session are serialized and dispatched outside library locks. Each desktop or mobile session runs codec
/// work and wakeups on its own worker; cleanup completion follows that worker's exit. On Web both callbacks run on
/// the owning event loop. Callbacks may race the return of Create() or a control call.
///
/// An asynchronous initialization or encoding failure starts teardown. TryRead() and TrySubmit() report that
/// failure until Close() changes the session to Closed. The optional cleanup callback reports teardown separately.
/// @see AudioEncoderConfig, AudioEncodeOutput, AudioDecoder
class AudioEncoder final {
public:
  /// @brief Validates the configuration and schedules platform initialization.
  /// @param config PCM format, target bitrate, queue limits, and optional callbacks, copied for the session.
  /// @return An owning encoder handle on success. Platform initialization may still be pending, in which case
  /// TrySubmit() returns WouldBlock. A synchronous Failure returns a CodecError and schedules no callbacks.
  /// Platform capability or initialization errors are subsequently reported through TryRead() and TrySubmit().
  /// @see Probe(), TryRead(), AudioEncoderConfig::wakeup
  static CodecResult<AudioEncoder> Create(const AudioEncoderConfig& config);

  /// @brief Tests the configuration asynchronously by initializing and releasing an independent platform encoder.
  ///
  /// Probe does not reserve resources for a later Create() or encode PCM. Platform output negotiation can still
  /// fail when a session starts processing input.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On desktop and mobile platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
  /// It must return promptly without throwing or waiting for encoder work.
  /// @see Create(), AudioEncoderConfig
  static void Probe(const AudioEncoderConfig& config, std::function<void(CodecResult<void>)> completion);

  /// @brief Asynchronously discovers the current adapter's AAC-LC encoding capabilities without encoding input.
  /// @param completion Optional owned-snapshot or query-error callback with the execution, lifetime, and exception
  /// contract of AudioDecoder::QueryCapabilities(). Probe the selected format and bitrate together before Create().
  /// @see AudioEncoderCapabilities, Probe(), AudioDecoder::QueryCapabilities()
  static void QueryCapabilities(std::function<void(CodecResult<AudioEncoderCapabilities>)> completion);

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  AudioEncoder(AudioEncoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle of a transfer then behaves as closed.
  AudioEncoder& operator=(AudioEncoder&&) noexcept;
  AudioEncoder(const AudioEncoder&) = delete;
  AudioEncoder& operator=(const AudioEncoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~AudioEncoder();

  /// @brief Queues one contiguous PCM interval without waiting for platform encoding.
  /// @param frame Valid AudioFrame with the configured format and a positive sample count. Keep the used plane
  /// bytes unchanged until return. PTS is rounded to the configured sample grid, using MediaTimestamp::Rescale();
  /// every frame after the first must begin immediately after the preceding accepted frame on that grid.
  /// Frames may contain any positive sample count that fits the input byte limit; they need not align to AAC units.
  /// @return Accepted after copying the used PCM bytes. WouldBlock leaves the frame and timeline unconsumed while
  /// startup or reset is pending or copied-input capacity is exhausted. Failure reports invalid input, copy resource
  /// exhaustion, InvalidState while draining or ended, Closed after retirement or on a moved-from handle, or the
  /// recorded operational error after fatal failure. Synchronous input rejection leaves the encoder available for
  /// valid PCM. Source planes can be reused as soon as Accepted returns.
  /// @see AudioFrame::Validate(), SubmitStatus, TryRead()
  CodecResult<SubmitStatus> TrySubmit(const AudioFrame& frame);

  /// @brief Removes the next output event without waiting for platform encoding.
  ///
  /// Format precedes its dependent packets. Packet PTS uses the configured sample grid on the accepted input
  /// timeline, including any platform preroll before input_start. DTS remains absent when the platform encoder does not
  /// report it. Each packet has a 1024-sample duration and is key-marked.
  ///
  /// Returned packet bytes are immutable and remain readable across Reset(), Close(), and cleanup completion.
  /// Copies and arbitrary slices retain the entire originating packet reservation until the last reference is
  /// released. Held packets can delay further encoding when the output pool is full.
  /// @return Failure with the recorded operational error or Closed. On success, std::nullopt means no event is
  /// available, including while a reset is pending. An engaged value carries one AudioEncodeFormat, EncodedPacket,
  /// AudioEncodeSummary, or EndOfStream. Summary and EndOfStream are consumed once per successful drain. Later reads
  /// return an empty optional until a new stream starts or the session closes.
  /// @see AudioEncodeOutput, AudioEncodeSummary, Finish()
  CodecResult<std::optional<AudioEncodeOutput>> TryRead();

  /// @brief Seals input and starts an asynchronous drain of accepted PCM.
  ///
  /// A partial final AAC input block is padded with silence. The sample summary excludes that fill from
  /// input_samples and includes it in any known padding_samples. Read remaining packets, then one summary and one
  /// EndOfStream. Held output can delay the drain until capacity is released. Further submissions return
  /// InvalidState while draining or ended. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset, the recorded operational error after fatal failure, or Closed after retirement or on a moved-from handle.
  /// @see AudioEncodeSummary, TryRead()
  CodecResult<void> Finish();

  /// @brief Discards pending input, output, and accounting and schedules a fresh stream with the same configuration.
  ///
  /// Discard takes effect on acceptance, including during a drain. Packets already returned or acquired by a
  /// concurrent read remain readable and hold their reservations. TrySubmit() returns WouldBlock until platform
  /// recreation finishes; TryRead() returns no stale events. Read recreation errors with TryRead(). The new stream
  /// can use a different starting PTS. Use Create() to change the PCM format, bitrate, or queue limits.
  /// @return Success when reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  /// @see Create(), AudioEncoderConfig
  CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Retirement ends further input, reads, and wakeup admission. A wakeup
  /// admitted before retirement may still run after return. Use cleanup_completed to observe platform release and
  /// callback quiescence before retiring state used by wakeup. Retained packet bytes do not delay completion.
  /// Allow the final callback to return and fence all codec work before unloading the module or terminating its worker.
  /// @see AudioEncoderConfig::cleanup_completed
  void Close() noexcept;
private:
  explicit AudioEncoder(std::shared_ptr<detail::AudioEncoderState> state);
  std::shared_ptr<detail::AudioEncoderState> state_;
};

/// @brief Move-only asynchronous H.264 and HEVC encoder with bounded queues and retained packet bytes.
///
/// Submit pictures matching the configured storage and layout and drain access units through TryRead().
/// The codec owns its session and any platform input surface, independently of a Runtime or UI window.
///
/// On desktop and mobile platforms, queue and control calls may run concurrently on a live handle. On Web, use the
/// JavaScript event loop that created the session. Move or destroy a handle only after its concurrent callers have
/// stopped. Codec operations can copy pixels and acquire locks, so schedule them outside hard real-time callbacks.
///
/// Callbacks for one session are serialized and dispatched outside library locks. Each desktop or mobile session runs
/// codec work and wakeups on its own worker; cleanup completion follows that worker's exit. On Web both callbacks
/// run on the owning event loop. Callbacks may race the return of Create() or a control call.
///
/// An asynchronous initialization or encoding failure starts teardown. TryRead() and TrySubmit() report that
/// failure until Close() changes the session to Closed. The optional cleanup callback reports teardown separately.
/// @see VideoEncoderConfig, VideoEncodeOutput, AudioEncoder
class VideoEncoder final {
public:
  /// @brief Validates the configuration and schedules platform initialization.
  /// @param config Input format, encoding controls, queue limits, and optional callbacks, copied for the session.
  /// @return An owning encoder handle on success. Platform initialization may still be pending, in which case
  /// TrySubmit() returns WouldBlock. A synchronous Failure returns a CodecError and schedules no callbacks.
  /// Platform capability or initialization errors are subsequently reported through TryRead() and TrySubmit().
  /// @see Probe(), TryRead(), VideoEncoderConfig::wakeup
  static CodecResult<VideoEncoder> Create(const VideoEncoderConfig& config);

  /// @brief Tests the configuration asynchronously by initializing and releasing an independent platform encoder.
  ///
  /// Probe does not reserve resources for a later Create() or encode application pictures. Platform output
  /// negotiation can still fail when a session starts processing input.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On desktop and mobile platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
  /// It must return promptly without throwing or waiting for encoder work.
  /// @see Create(), VideoEncoderConfig
  static void Probe(const VideoEncoderConfig& config, std::function<void(CodecResult<void>)> completion);

  /// @brief Asynchronously discovers the current adapter's CPU H.264 encoding capabilities without encoding input.
  /// @param completion Optional owned-snapshot or query-error callback with the execution, lifetime, and exception
  /// contract of AudioDecoder::QueryCapabilities(). Probe profile, B policy, input geometry, rate, and bitrate together.
  /// @see VideoEncoderCapabilities, Probe(), AudioDecoder::QueryCapabilities()
  static void QueryCapabilities(std::function<void(CodecResult<VideoEncoderCapabilities>)> completion);

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  VideoEncoder(VideoEncoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle then behaves as closed.
  VideoEncoder& operator=(VideoEncoder&&) noexcept;
  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~VideoEncoder();

  /// @brief Queues one picture without waiting for platform encoding.
  /// @param frame Valid VideoFrame matching the configured input geometry, layout, and storage. Keep CPU planes
  /// stable through the call. For platform images, complete producer writes before submitting and keep the image
  /// immutable while retained. PTS must increase strictly in exact time and after microsecond rounding.
  /// Platform conversion and range limits are checked separately.
  /// Optional duration must be positive and representable when present. Variable frame intervals are allowed.
  /// @return Accepted after copying CPU samples or retaining the platform image. WouldBlock leaves the frame unconsumed
  /// while startup or reset is pending or input capacity is exhausted. Failure reports invalid input, resource
  /// exhaustion, InvalidState while draining or ended, Closed after retirement or on a moved-from handle, or the
  /// recorded operational error after fatal failure. Synchronous rejection leaves the session usable for valid
  /// input. Source planes can be reused as soon as Accepted returns.
  /// @see VideoFrame::Validate(), SubmitStatus, TryRead()
  CodecResult<SubmitStatus> TrySubmit(const VideoFrame& frame);

  /// @brief Removes the next output event without waiting for platform encoding.
  ///
  /// Format precedes its dependent packets. Each packet contains one picture with four-byte NAL lengths and is
  /// emitted in decode order; PTS may move backward with B pictures. The original input PTS and supplied duration
  /// are restored to the corresponding picture. DTS uses reliable platform timing and remains absent when unavailable.
  ///
  /// Returned packet bytes are immutable and remain readable across Reset(), Close(), and cleanup completion.
  /// Copies and arbitrary slices retain the entire originating packet reservation until the last reference is
  /// released. Held packets can delay further encoding when the output pool is full.
  /// @return Failure with the recorded operational error or Closed. On success, std::nullopt means no event is
  /// available, including while a reset is pending. An engaged value carries one VideoEncodeFormat, EncodedPacket,
  /// or EndOfStream. EndOfStream is consumed once per successful drain. Later reads return an empty optional until
  /// a new stream starts or the session closes.
  /// @see VideoEncodeOutput, VideoEncodeFormat, Finish()
  CodecResult<std::optional<VideoEncodeOutput>> TryRead();

  /// @brief Requests a new random-access picture as soon as the platform can produce one.
  ///
  /// Repeated pending requests may coalesce. The request does not promise a particular input frame or fixed
  /// response distance. Observe actual packet key_frame values to identify IDR pictures.
  /// @return Success when accepted in the running phase. Other phases report InvalidState, the recorded operational
  /// error, or Closed. Platform rejection is subsequently reported through TryRead() and TrySubmit().
  /// @see EncodedPacket::key_frame, VideoEncoderConfig::key_frame_interval
  CodecResult<void> RequestKeyFrame();

  /// @brief Seals input and drains all accepted and reordered pictures before one EndOfStream.
  ///
  /// Read remaining packets, then one EndOfStream. Held output can delay the drain until capacity is released.
  /// An empty stream can end without a format event. Further submissions return InvalidState while draining or
  /// ended. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset, the recorded operational error after fatal failure, or Closed after retirement or on a moved-from handle.
  /// @see VideoEncodeOutput, TryRead()
  CodecResult<void> Finish();

  /// @brief Discards pending input and output and schedules a fresh stream with the same configuration.
  ///
  /// Discard takes effect on acceptance, including during a drain. Returned packets retain storage and reservations
  /// in the same output pool. TrySubmit() returns WouldBlock until platform recreation finishes; TryRead() returns
  /// no stale events. The new stream starts with IDR and can use a different starting PTS. Use Create() to change
  /// the input format, encoding controls, or queue limits.
  /// @return Success when reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  /// @see Create(), VideoEncoderConfig
  CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Retirement ends further input, reads, and wakeup admission. A wakeup
  /// admitted before retirement may still run after return. Use cleanup_completed to observe platform release and
  /// callback quiescence before retiring state used by wakeup. Retained packet bytes do not delay completion.
  /// Allow the final callback to return and fence all codec work before unloading the module or terminating its worker.
  /// @see VideoEncoderConfig::cleanup_completed
  void Close() noexcept;
private:
  explicit VideoEncoder(std::shared_ptr<detail::VideoEncoderState> state);
  std::shared_ptr<detail::VideoEncoderState> state_;
};

/// @brief Creates a HuxerUI texture containing one platform video frame.
///
/// The texture remains usable after the input frame is released and the decoder is reset or closed.
/// macOS, iOS, Android, and Windows textures contain the full coded image. Use the frame's format to crop or rotate it
/// for display. Web textures preserve the browser frame's visible rectangle and use its display dimensions as the
/// texture's intrinsic size. The browser frame can include allocation padding beyond the configured coded dimensions.
/// Use the VideoFrame for the media timestamp and any additional presentation metadata.
///
/// Include the SDK platform texture header to use the concrete macos::PixelBufferTexture,
/// ios::PixelBufferTexture, android::GlTexture, windows::D3D11Texture, or web::VideoFrameTexture type.
/// Apple and Web textures retain the source image and its decoder output credit. Android and Windows create a GPU
/// snapshot in RGBA8 and BGRA8 respectively. Keep the texture alive through GPU use and treat the acquired pixels
/// as read-only. Create, use, and release Web textures on the browser main thread.
///
/// @param frame Valid eight-bit SDR frame with VideoStorage::Platform. Producer writes must have completed.
/// @return A texture with its frame published and further publication closed. InvalidArgument reports
/// an invalid or CPU frame. Unsupported reports an unavailable backend or unsupported platform image.
CodecResult<std::shared_ptr<ExternalTexture>> MakeExternalTexture(const VideoFrame& frame);

/// @brief Retains one published SDK texture image as a platform VideoFrame.
///
/// Complete producer writes according to the SDK texture's publication contract. Serialize publication and acquisition
/// to associate the intended image with its PTS. Later publications do not replace the returned frame. It retains the
/// acquired image and texture ownership, including any attached decoder reservation. Keep the retained pixels immutable.
/// Use the same frame for retries when VideoEncoder::TrySubmit() returns WouldBlock.
///
/// Supports SDR NV12 from macos::PixelBufferTexture, ios::PixelBufferTexture, and windows::D3D11Texture. Windows
/// synchronously converts an opaque BGRA8 snapshot to NV12 on the encoder's graphics adapter and requires even
/// dimensions. SDR Opaque is supported from android::GlTexture and web::VideoFrameTexture. On Web, acquire and
/// release the frame on the browser main thread.
/// @param texture Shared owner of an SDK texture with a completed publication.
/// @param format Pixel geometry, layout, and color metadata for the image. Use physical pixels, independent of the
/// texture's logical size. For Web, describe the visible image, excluding browser allocation padding.
/// @param pts Presentation time on the media timeline, with a positive timescale.
/// @param duration Optional positive presentation duration with a positive timescale.
/// @return A retained frame with empty CPU planes. InvalidState indicates that the texture has not published a frame.
/// InvalidArgument reports missing or inconsistent input. Unsupported reports an unavailable texture type or layout.
/// ResourceExhausted reports allocation failure or a Windows snapshot that remained busy during acquisition.
/// @see MakeExternalTexture(), VideoEncoder::TrySubmit(), VideoEncoderConfig::input_storage
CodecResult<VideoFrame> MakeVideoFrame(std::shared_ptr<const ExternalTexture> texture, VideoFormat format,
    MediaTimestamp pts, std::optional<MediaTimestamp> duration = std::nullopt);

} // namespace huxerui::media
