#pragma once

/// @file
/// @brief Headless, bounded, asynchronous AAC-LC encoding and AAC-LC/H.264 decoding.
///
/// Include <huxerui/media_codec.h> and link HuxerUI::Media. Use these sessions directly from headless code.
/// Decoders accept initialization data and complete compressed access units in decode order, then emit format
/// events and retained CPU frames. The audio encoder accepts contiguous PCM and emits AAC configuration, retained
/// raw access units, and sample accounting. A successful drain ends with one EndOfStream event.

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <huxerui/media_format.h>

namespace huxerui::media {
namespace detail {
template<class Output> class DecoderState;
class AudioEncoderState;
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
/// Input capacity counts copied compressed packets for decoding or PCM frames for encoding, including bytes still
/// retained by an adapter. It becomes available when the last owner releases the copied storage. Output capacity is
/// reserved before platform conversion and covers in-flight, reordered, queued, and consumer-held frames or packets.
/// Retaining a plane or slice keeps the entire originating output reservation, including across Reset(). Release
/// retained output to let pending input advance when the output pool is full.
///
/// Decoders require at least two input slots and a positive input byte limit. Audio decoding requires four output
/// slots sized for 1024 samples per channel. Video requires 34 output slots sized for the coded bounds and 4:2:0 planes.
/// Count one byte per coded luma sample and two bytes per subsampled chroma position for each video slot.
/// Both output limits must accommodate the required slots. Platform codec and adapter scratch allocations are separate.
/// AudioEncoderConfig specifies the separate PCM input and compressed output minimums for encoding.
/// @see AudioDecoderConfig::limits, VideoDecoderConfig::limits, AudioEncoderConfig::limits
struct QueueLimits {
  /// Maximum number of copied input packets or PCM frames still retained. Defaults to 48.
  std::size_t input_items = 48;
  /// Maximum total retained input bytes. A larger single packet or PCM frame is rejected. Defaults to 8 MiB.
  std::size_t input_bytes = 8 * 1024 * 1024;
  /// Maximum number of output frame or packet reservations. Defaults to 48, subject to output_bytes.
  std::size_t output_items = 48;
  /// Maximum total bytes reserved for output. Decoders charge the maximum configured frame size per slot.
  /// Audio encoding reserves 64 KiB per pending packet, then reduces the charge to its actual payload size.
  /// The default is 192 MiB; individual configurations can supply smaller defaults. Storage is allocated as needed.
  std::size_t output_bytes = 192 * 1024 * 1024;
};

/// @brief Configuration for decoding raw AAC-LC access units to CPU PCM.
///
/// Supply AAC-LC with 1024 samples per channel per access unit and a mono or stereo layout. The decoder uses
/// the stream's sample rate and channel layout. Choose PCM packing and sample representation in output.
/// @see AudioDecoder::Create(), AudioDecoder::Probe()
struct AudioDecoderConfig {
  /// Two-byte MPEG-4 AudioSpecificConfig for the raw AAC-LC access units supplied to TrySubmit().
  Bytes initialization_data;
  /// Requested PCM format. Set the sample rate and channel layout to match initialization_data.
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
};

/// @brief Configuration for decoding length-prefixed H.264 access units to CPU YUV.
///
/// The initial subset is progressive 8-bit 4:2:0 Baseline, Main, and High profiles, with at most 16 reference
/// pictures and continuous frame numbering. Probe the concrete configuration for platform support.
/// @see VideoDecoder::Create(), VideoDecoder::Probe()
struct VideoDecoderConfig {
  /// AVCDecoderConfigurationRecord (avcC) of at most 64 KiB. Include SPS and PPS data and a NAL length field
  /// size of 1, 2, or 4 bytes. Every submitted picture must use that length prefix size.
  Bytes initialization_data;
  /// Maximum coded luma width in pixels, in [1, 8192]. The backend can impose smaller limits.
  /// Together with height, configures the codec and sizes output reservations. Use a new session for larger bounds.
  std::uint32_t width = 0;
  /// Maximum coded luma height in pixels, in [1, 8192], subject to the platform limit described for width.
  std::uint32_t height = 0;
  /// Requested I420 or NV12 CPU layout. An unavailable layout produces an error.
  VideoPixelFormat output = VideoPixelFormat::NV12;
  /// Retained input and output bounds. See QueueLimits for the video minimum and byte accounting.
  QueueLimits limits;
  /// Optional readiness callback with the delivery, capture, and execution contract of AudioDecoderConfig::wakeup.
  std::function<void()> wakeup;
  /// Optional teardown callback with the resource and lifetime contract of AudioDecoderConfig::cleanup_completed.
  std::function<void(CodecResult<void>)> cleanup_completed;
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
/// @see AudioDecoder::Finish(), VideoDecoder::Finish(), AudioEncoder::Finish()
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

/// @brief Move-only AAC-LC decoder with bounded queues and independently retained CPU output.
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

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  AudioDecoder(AudioDecoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle of a transfer then behaves as closed.
  AudioDecoder& operator=(AudioDecoder&&) noexcept;
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~AudioDecoder();

  /// @brief Queues one raw AAC access unit without waiting for platform decoding.
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

/// @brief Move-only H.264 decoder with bounded queues and retained CPU YUV frames.
///
/// Submit complete picture access units in decode order and begin each stream with a key-marked IDR. Output
/// arrives in presentation order. The current 16-frame reordering window can hold frames until more input arrives
/// or Finish() drains it. See AudioDecoder for the shared execution, callback, and handle ownership contract.
/// @see VideoDecoderConfig, VideoFrame, AudioDecoder
class VideoDecoder final {
public:
  /// @brief Validates the AVC configuration and schedules platform initialization.
  /// @param config avcC data, coded bounds, output layout, queue limits, and optional callbacks, copied for the session.
  /// @return An owning decoder handle on success. TrySubmit() can return WouldBlock while initialization is pending.
  /// A synchronous Failure returns a CodecError and schedules no callbacks.
  /// @see Probe(), TryRead(), VideoDecoderConfig::wakeup
  static CodecResult<VideoDecoder> Create(const VideoDecoderConfig& config);

  /// @brief Tests platform initialization and release for the supplied AVC configuration asynchronously.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On desktop and mobile platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
  /// It must return promptly without throwing or waiting for decoder work.
  /// @see AudioDecoder::Probe()
  static void Probe(const VideoDecoderConfig& config, std::function<void(CodecResult<void>)> completion);

  /// @brief Transfers ownership of the session and its callbacks. The source handle then behaves as closed.
  VideoDecoder(VideoDecoder&&) noexcept;
  /// @brief Retires the destination's previous session and takes ownership of the source session.
  /// @return This handle. The source handle of a transfer then behaves as closed.
  VideoDecoder& operator=(VideoDecoder&&) noexcept;
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  /// @brief Retires the session as in Close(). Internal state remains alive through asynchronous teardown.
  ~VideoDecoder();

  /// @brief Queues one complete length-prefixed AVC picture access unit without waiting for platform decoding.
  /// @param packet Access unit in decode order, with valid timing and NAL lengths matching initialization_data.
  /// The first packet after creation or reset must contain an IDR and set key_frame. Keep bytes unchanged until return.
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

  /// @brief Discards pending input and output and schedules a fresh AVC stream with the same configuration.
  ///
  /// Previously returned frames retain storage and reservations. TrySubmit() returns WouldBlock until the new
  /// stream is ready. Start it with a key-marked IDR. Use Create() to change coded bounds or initialization data.
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

} // namespace huxerui::media
