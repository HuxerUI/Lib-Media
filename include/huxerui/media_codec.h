#pragma once

/// @file
/// @brief Headless, bounded, asynchronous AAC-LC and H.264 decoding.
///
/// Include <huxerui/media_codec.h> and link HuxerUI::Media. Use the decoders directly from headless code.
/// The application supplies decoder initialization data and complete compressed access units in decode order.
/// Decoder output consists of format events, retained CPU frames, and an end-of-stream event.

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <huxerui/media_format.h>

namespace huxerui::media {
namespace detail { template<class Output> class DecoderState; }

/// @brief Portable categories for configuration, decoding, and cleanup errors.
///
/// A full input queue reports the successful status SubmitStatus::WouldBlock.
enum class CodecErrorCode {
  /// The codec, profile, CPU layout, or platform capability is unavailable.
  Unsupported,
  /// A configuration, queue limit, or declared stream bound is invalid.
  InvalidArgument,
  /// The operation is not permitted in the current lifecycle phase.
  InvalidState,
  /// An access unit, timestamp, or native decoded value violates the configured contract.
  InvalidData,
  /// A bounded allocation or native resource request could not be satisfied.
  ResourceExhausted,
  /// The native API failed without a more specific portable category.
  PlatformFailure,
  /// The session has been logically retired, or the handle has been moved from.
  Closed
};

/// @brief A codec failure with a portable category and optional native diagnostic information.
struct CodecError {
  /// Stable category for application error handling.
  CodecErrorCode code = CodecErrorCode::PlatformFailure;
  /// Human-readable diagnostic text. Use code for application decisions.
  std::string message;
  /// Native error number or subsystem identifier when available. An empty string means unavailable.
  std::string platform_code;
};

/// @brief SDK result carrying an owned success value or a CodecError.
///
/// Check Succeeded() before accessing Value() or Error(). Accessing the unavailable branch throws std::logic_error.
/// References from those accessors borrow the result. Copy or move the stored value to keep it independently.
/// @tparam T Success type. Use void for operations that confirm acceptance or completion without a payload.
template<class T> using CodecResult = Result<T, CodecError>;

/// @brief Limits on retained compressed input and decoded frame reservations for one session.
///
/// Input capacity counts packets queued by the library or still retained by an adapter. It becomes available when
/// the last owner of the copied bytes releases them. Output capacity is reserved before native conversion and covers
/// in-flight, reordered, queued, and consumer-held frames. A retained plane or slice keeps its full frame reservation
/// even after Reset(). Release retained output to let pending input advance when the output pool is full.
///
/// Supply at least two input slots and a positive input byte limit. Audio requires four output slots sized for
/// 1024 samples per channel. Video requires 34 output slots sized for the configured coded bounds and 4:2:0 planes.
/// Count one byte per coded luma sample and two bytes per subsampled chroma position for each video slot.
/// Both output limits must accommodate the required slots. Native codec and adapter scratch allocations are separate.
/// @see AudioDecoderConfig::limits, VideoDecoderConfig::limits
struct QueueLimits {
  /// Maximum number of copied packets still queued or retained by the adapter. Defaults to 48.
  std::size_t input_items = 48;
  /// Maximum total bytes of retained compressed payload. A larger single packet is rejected. Defaults to 8 MiB.
  std::size_t input_bytes = 8 * 1024 * 1024;
  /// Maximum number of frame reservations. Defaults to 48, subject to output_bytes.
  std::size_t output_items = 48;
  /// Maximum total bytes reserved for frames. Each slot is charged for the maximum configured frame size.
  /// The default is 192 MiB. Storage is allocated as needed.
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
  /// It follows all admitted wakeups and, on native platforms, the exit of the session's worker.
  /// Success confirms native resources have been released or safely detached at the codec API boundary.
  /// The driver or browser can reclaim its own storage later. Failure reports a cleanup error independently of
  /// any decoding error. Native work that cannot safely be retired leaves it pending.
  /// Retained CPU frames remain readable and do not delay completion. The callback must return promptly and not throw.
  std::function<void(CodecResult<void>)> cleanup_completed;
};

/// @brief Configuration for decoding length-prefixed H.264 access units to CPU YUV.
///
/// The initial subset is progressive 8-bit 4:2:0 Baseline, Main, and High profiles, with at most 16 reference
/// pictures and continuous frame numbering. Probe the concrete configuration for native support.
/// @see VideoDecoder::Create(), VideoDecoder::Probe()
struct VideoDecoderConfig {
  /// AVCDecoderConfigurationRecord (avcC) of at most 64 KiB. Include SPS and PPS data and a NAL length field
  /// size of 1, 2, or 4 bytes. Every submitted picture must use that length prefix size.
  Bytes initialization_data;
  /// Maximum coded luma width and height in pixels, each in [1, 8192]. The backend can impose smaller limits.
  /// These dimensions configure the codec and size output reservations. Use a new session for larger coded images.
  std::uint32_t width = 0, height = 0;
  /// Requested I420 or NV12 CPU layout. An unavailable layout produces an error.
  VideoPixelFormat output = VideoPixelFormat::NV12;
  /// Retained input and output bounds. See QueueLimits for the video minimum and byte accounting.
  QueueLimits limits;
  /// Optional readiness callback with the delivery, capture, and execution contract of AudioDecoderConfig::wakeup.
  std::function<void()> wakeup;
  /// Optional teardown callback with the resource and lifetime contract of AudioDecoderConfig::cleanup_completed.
  std::function<void(CodecResult<void>)> cleanup_completed;
};

/// @brief End of a successfully drained stream.
///
/// The decoder emits this once after Finish() and all delayed frames. The session remains open for Reset() or Close().
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

/// @brief Move-only AAC-LC decoder with bounded queues and independently retained CPU output.
///
/// The returned handle owns the session. On native platforms, queue and control calls may run concurrently on a live
/// handle. On Web, use the JavaScript event loop that created the session. Move or destroy a handle only after its
/// concurrent callers have stopped. Codec operations can copy bytes and acquire locks, so schedule them outside hard
/// real-time callbacks.
///
/// Callbacks for one session are serialized and dispatched outside library locks. Each native session runs codec
/// work and wakeups on its own worker. Cleanup completion is dispatched after that worker exits. These callbacks
/// may race the return of Create() or a control call. On Web both callbacks run on the owning event loop.
///
/// An asynchronous initialization or decoding failure starts teardown. TryRead() and TrySubmit() report that
/// failure until Close() changes the session to Closed. The optional cleanup callback reports teardown separately.
/// @see AudioDecoderConfig, VideoDecoder
class AudioDecoder final {
public:
  /// @brief Validates the configuration and schedules native initialization.
  /// @param config AAC setup data, PCM format, queue limits, and optional callbacks, copied for the session.
  /// @return An owning decoder handle on success. Initialization may still be pending, in which case TrySubmit()
  /// returns WouldBlock. A synchronous Failure returns a CodecError and schedules no callbacks.
  /// @see Probe(), TryRead(), AudioDecoderConfig::wakeup
  [[nodiscard]] static CodecResult<AudioDecoder> Create(const AudioDecoderConfig& config);

  /// @brief Tests the configuration asynchronously by initializing and releasing an independent native codec.
  ///
  /// The probe uses its own completion. A later Create() acquires its own resources and validates input during decode.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On native platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
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

  /// @brief Queues one raw AAC access unit without waiting for native decoding.
  /// @param packet Complete access unit with a nonempty payload and valid timing. Submit packets in decode order
  /// with nondecreasing PTS and keep the bytes unchanged until the call returns.
  /// @return Accepted after copying the packet. WouldBlock leaves it unconsumed while startup or reset is pending
  /// or input capacity is exhausted. Failure reports packet rejection, InvalidState while draining or ended,
  /// Closed after retirement or on a moved-from handle, or the recorded operational error after fatal failure.
  /// A packet rejected by synchronous validation leaves the decoder available for valid input.
  /// @see EncodedPacket, SubmitStatus, TryRead()
  [[nodiscard]] CodecResult<SubmitStatus> TrySubmit(const EncodedPacket& packet);

  /// @brief Removes the next output event without waiting for native decoding.
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
  [[nodiscard]] CodecResult<std::optional<AudioDecodeOutput>> TryRead();

  /// @brief Seals input and starts an asynchronous drain of accepted packets.
  ///
  /// Read remaining frames and then EndOfStream through TryRead(). Held output can delay the drain until capacity
  /// is released. Further submissions return InvalidState while draining or ended. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset or after a fatal error. Closed after retirement or on a moved-from handle.
  [[nodiscard]] CodecResult<void> Finish();

  /// @brief Discards pending input and output and schedules a fresh stream with the same configuration.
  ///
  /// The discard takes effect on acceptance, including during a drain. Frames already returned or acquired by a
  /// concurrent read before the reset remain readable and hold their reservations. TrySubmit() returns WouldBlock
  /// until native recreation finishes. Read any recreation error with TryRead(). Use Create() for a new configuration.
  /// @return Success when the reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  [[nodiscard]] CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Retirement ends further input, reads, and wakeup admission. A wakeup
  /// admitted before retirement may still run after return. Use cleanup_completed to observe native release and
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
  /// @brief Validates the AVC configuration and schedules native initialization.
  /// @param config avcC data, coded bounds, output layout, queue limits, and optional callbacks, copied for the session.
  /// @return An owning decoder handle on success. TrySubmit() can return WouldBlock while initialization is pending.
  /// A synchronous Failure returns a CodecError and schedules no callbacks.
  /// @see Probe(), TryRead(), VideoDecoderConfig::wakeup
  [[nodiscard]] static CodecResult<VideoDecoder> Create(const VideoDecoderConfig& config);

  /// @brief Tests native initialization and release for the supplied AVC configuration asynchronously.
  /// @param config Configuration to probe. The wakeup and cleanup_completed fields are ignored.
  /// @param completion Optional callback invoked once when the probe finishes, with success or a CodecError.
  /// On native platforms it is dispatched after the probe worker exits; on Web it runs on the owning event loop.
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

  /// @brief Queues one complete length-prefixed AVC picture access unit without waiting for native decoding.
  /// @param packet Access unit in decode order, with valid timing and NAL lengths matching initialization_data.
  /// The first packet after creation or reset must contain an IDR and set key_frame. Keep bytes unchanged until return.
  /// @return Accepted after copying the packet. WouldBlock leaves it unconsumed while startup or reset is pending
  /// or input capacity is exhausted. Failure reports packet rejection, InvalidState while draining or ended,
  /// Closed after retirement or on a moved-from handle, or the recorded operational error after fatal failure.
  /// @see EncodedPacket, SubmitStatus, AudioDecoder::TrySubmit()
  [[nodiscard]] CodecResult<SubmitStatus> TrySubmit(const EncodedPacket& packet);

  /// @brief Removes the next output event without waiting for native decoding or reordering.
  /// @return Failure with the recorded operational error or Closed. On success, std::nullopt means no event is
  /// available. An engaged value carries one VideoFormat, VideoFrame, or EndOfStream. Format events precede
  /// their dependent frames. EndOfStream is delivered once per successful drain, then reads are empty.
  /// @see VideoDecodeOutput, VideoFrame, AudioDecoder::TryRead()
  [[nodiscard]] CodecResult<std::optional<VideoDecodeOutput>> TryRead();

  /// @brief Seals input and asynchronously drains native and reordered frames before EndOfStream.
  ///
  /// Continue reading and releasing frames to allow the drain to complete. The session remains open for Reset().
  /// @return Success when the drain is requested, already pending, or complete. InvalidState during startup or
  /// reset or after a fatal error. Closed after retirement or on a moved-from handle.
  /// @see AudioDecoder::Finish()
  [[nodiscard]] CodecResult<void> Finish();

  /// @brief Discards pending input and output and schedules a fresh AVC stream with the same configuration.
  ///
  /// Previously returned frames retain storage and reservations. TrySubmit() returns WouldBlock until the new
  /// stream is ready. Start it with a key-marked IDR. Use Create() to change coded bounds or initialization data.
  /// @return Success when the reset is accepted or already pending. InvalidState during startup or after a fatal
  /// error. Closed after retirement or on a moved-from handle.
  /// @see AudioDecoder::Reset()
  [[nodiscard]] CodecResult<void> Reset();

  /// @brief Immediately retires the session and schedules asynchronous teardown.
  ///
  /// Safe to repeat or call from wakeup. Earlier admitted callbacks may still run after return. Observe native
  /// release and callback quiescence through cleanup_completed. Previously returned CPU frames remain readable.
  /// @see VideoDecoderConfig::cleanup_completed, AudioDecoder::Close()
  void Close() noexcept;
private:
  explicit VideoDecoder(std::shared_ptr<detail::DecoderState<VideoDecodeOutput>> state);
  std::shared_ptr<detail::DecoderState<VideoDecodeOutput>> state_;
};

} // namespace huxerui::media
