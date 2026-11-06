#pragma once

/// @file
/// @brief Headless playback of decoded PCM through a native audio device.
///
/// Include <huxerui/media_playback.h> and link HuxerUI::Media. Create a session with a fixed PCM format and
/// submit AudioFrame values from a decoder or another producer. The session owns the output device and the
/// playback timeline. The producer owns source loading, decoding, seeking and any input format conversion.
///
/// PCM playback is available on Android API 26+ and macOS, subject to the output route and requested format.
/// Use the API independently of a View, Runtime or window. The application manages audio focus and background
/// playback policy. See AudioPlaybackConfig for supported formats and callback delivery.

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <huxerui/media_format.h>

namespace huxerui::media {
namespace detail { class PlaybackSession; }

/// @brief Portable categories for playback setup, input, device and cleanup failures.
///
/// A temporarily full queue returns a successful SubmitStatus::WouldBlock result. An operational failure
/// appears in PlaybackSnapshot::error, while AudioPlaybackConfig::cleanup_completed reports device release.
enum class PlaybackErrorCode {
  /// The platform, PCM format or device capability is unavailable.
  Unsupported,
  /// A configuration bound, PCM frame, timestamp or gain is invalid.
  InvalidArgument,
  /// The current transport or input state prevents the requested operation.
  InvalidState,
  /// The session could not allocate playback storage or start its worker.
  ResourceExhausted,
  /// A native operation failed or the route stopped providing reliable presentation progress.
  DeviceFailure,
  /// The session is retired or the handle has been moved from.
  Closed
};

/// @brief A playback error with optional native diagnostic information.
struct PlaybackError {
  /// Portable category for application error handling.
  PlaybackErrorCode code = PlaybackErrorCode::DeviceFailure;
  /// Human-readable diagnostic for the failed operation.
  std::string message;
  /// Native error identifier when available. Empty when the backend supplies none.
  std::string platform_code;
};

/// @brief An owned result from a playback operation.
///
/// Check Succeeded() before reading Value() or Error(). Accessing the unavailable branch throws std::logic_error.
/// References from those accessors borrow the result. Copy or move the stored value to keep it independently.
/// @tparam T Success value. Use void for operations that report acceptance or completion without a payload.
template<class T> using PlaybackResult = Result<T, PlaybackError>;

/// @brief Observable transport state for one playback timeline.
enum class PlaybackState {
  /// Initial device opening is pending. Submission returns WouldBlock.
  Starting,
  /// Playback is ready or the requested pause took effect. The reported position is frozen.
  Paused,
  /// Play is requested and the session is waiting for enough media to start or refill.
  Buffering,
  /// The session has started device output. This includes the period in which sealed input drains.
  Playing,
  /// The device confirmed all accepted samples and output is paused. Reset to start another epoch.
  Ended,
  /// The old epoch is retired and the device is being recreated. Submission returns WouldBlock.
  Resetting,
  /// An operational failure is available in the snapshot. Cleanup starts automatically.
  Failed,
  /// The handle is retired. Native cleanup may still be pending.
  Closed
};

/// @brief Configuration for a session that plays interleaved S16 mono or stereo PCM.
struct AudioPlaybackConfig {
  /// Exact format for every frame in the session. Use a positive sample rate and interleaved S16 mono or stereo.
  /// Device support for that sample rate and channel layout is checked during opening.
  AudioFormat format;

  /// Maximum duration of accepted media still awaiting presentation, including PCM handed to the device.
  /// Supply a positive duration. The session rounds up to whole sample frames. A frame larger than this entire
  /// capacity is invalid. Device buffering and conversion scratch are separately bounded.
  std::chrono::milliseconds buffer_capacity{250};

  /// Target duration to start or refill playback. Supply a positive duration at most buffer_capacity.
  /// A valid frame blocked by capacity can trigger an earlier start. Sealing input also allows a short tail to play.
  std::chrono::milliseconds refill_threshold{100};

  /// Optional coalesced signal that state, position or input capacity may have changed.
  /// Read Snapshot() or schedule the producer to retry pending input. The session invokes this callback on
  /// its worker outside session locks. It may run before Create() returns, so capture independently owned or weak
  /// state. It may call Close(). Return promptly without throwing or waiting for the worker or cleanup.
  std::function<void()> wakeup;

  /// Optional final result delivered once after Close(), destruction or an operational failure.
  /// Delivery follows native release, the return of admitted wakeups and worker exit. Success confirms release.
  /// Failure describes a release error independently of any operational error. Delivery remains pending while
  /// unsafe native work is still outstanding. The callback runs off the caller's stack.
  /// Use captures that survive retirement. Return promptly without throwing or waiting for other media work.
  std::function<void(PlaybackResult<void>)> cleanup_completed;
};

/// @brief A consistent snapshot of the current epoch and its observable transport state.
struct PlaybackSnapshot {
  /// Last published transport state. A default snapshot describes a closed handle.
  PlaybackState state = PlaybackState::Closed;
  /// Boundary after the presented media samples, expressed in ticks at the configured sample rate.
  /// Empty until the first frame of an epoch is accepted. Initially equals that frame's rescaled PTS, which
  /// can be negative. It advances with device-confirmed media, freezes when paused or starved, and ignores
  /// silence inserted by the device adapter. Update granularity depends on the output route.
  std::optional<MediaTimestamp> position;
  /// Duration of accepted media awaiting presentation, including samples submitted to the device.
  /// Expressed at the configured sample rate. Failed and closed sessions report zero queued duration.
  MediaTimestamp buffered_duration;
  /// Number of starvation gaps confirmed after playback started in this epoch. Reset() clears the count.
  std::uint64_t underruns = 0;
  /// True once FinishAudio() seals the epoch. Use state to observe when the sealed input has played.
  bool input_finished = false;
  /// Operational failure while state is Failed. The cleanup callback carries any separate release error.
  std::optional<PlaybackError> error;
};

/// @brief A move-only session for bounded decoded audio playback.
///
/// Wait for opening to finish, then submit a contiguous sequence of frames and call Play(). Retry the same
/// frame after WouldBlock. A registered wakeup can schedule retries as capacity returns. Seal input with
/// FinishAudio() and use Snapshot() to observe Ended. Reset() retires the timeline for another stream or seek.
///
/// Controls, submission and snapshots may run concurrently on a live handle. Concurrent submissions are ordered
/// by acceptance. Move or destroy the handle after its callers stop. Audio controls acquire locks and submission
/// copies bytes. Call outside real-time render callbacks. The session serializes device work on its own worker.
/// @see AudioPlaybackConfig, AudioFrame, PlaybackSnapshot
class DecodedPlayback final {
public:
  /// @brief Validate the configuration and schedule asynchronous device opening.
  ///
  /// Opening begins in Starting and publishes Paused when the device is ready. Native open errors publish
  /// Failed and initiate cleanup. Opening and wakeups can race the return of Create().
  /// @param config Exact PCM format, queue limits and optional callbacks, copied into the session.
  /// @return An owning handle, or a synchronous Unsupported, InvalidArgument or ResourceExhausted error.
  /// A synchronous failure schedules no callbacks.
  /// @see Snapshot(), AudioPlaybackConfig::wakeup
  [[nodiscard]] static PlaybackResult<DecodedPlayback> Create(const AudioPlaybackConfig& config);

  /// @brief Transfer ownership of a session and its callbacks.
  /// The source handle behaves as closed after the transfer.
  DecodedPlayback(DecodedPlayback&&) noexcept;

  /// @brief Retire the destination's session and take ownership of the source.
  /// The source handle behaves as closed after the transfer. Self-assignment preserves ownership.
  /// @return This handle.
  DecodedPlayback& operator=(DecodedPlayback&&) noexcept;

  /// @brief Retire the owned session using the asynchronous Close() contract.
  /// Captured callback state must remain safe through cleanup completion.
  ~DecodedPlayback();

  /// @brief Copy a complete PCM frame into the bounded playback queue.
  ///
  /// The first accepted PTS anchors the epoch. Each later PTS must match the end of the previous accepted
  /// frame after rescaling to the configured sample rate. Reset the epoch before submitting a discontinuity.
  /// Capacity counts both software PCM and device submissions until presentation. If a valid frame cannot fit
  /// in the remaining space, a pending play request can start queued media below the usual refill threshold.
  /// @param frame Valid frame in the configured format. Keep its plane bytes stable throughout the call.
  /// @return Accepted after copying the whole frame, after which the producer can release the source bytes.
  /// WouldBlock consumes no bytes during opening, reset or temporary saturation. Retry the same frame when
  /// the session can accept more. InvalidArgument covers malformed PCM, timestamp discontinuities and a frame
  /// larger than the entire capacity. Sealed input returns InvalidState. Failed and closed sessions report
  /// their operational error or Closed, respectively.
  /// @see AudioFrame::Validate(), AudioPlaybackConfig::buffer_capacity, FinishAudio(), Reset()
  [[nodiscard]] PlaybackResult<SubmitStatus> TrySubmitAudio(const AudioFrame& frame);

  /// @brief Request playback or record play intent while opening or resetting.
  ///
  /// Startup and refill wait for available PCM and the configured threshold. A sealed short tail or capacity
  /// pressure can start sooner. Resuming a paused device can continue with its pending media.
  /// @return Success when play is requested or already intended. Inspect Snapshot() for the observed device
  /// state. Ended requires Reset() and returns InvalidState. Failed and closed sessions return an error.
  /// @see Pause(), Snapshot(), AudioPlaybackConfig::refill_threshold
  [[nodiscard]] PlaybackResult<void> Play();

  /// @brief Clear play intent and request a pause while keeping queued media.
  ///
  /// Paused reports that the native pause took effect and the published position is frozen. Device progress
  /// already admitted may still advance before that boundary. During opening or reset, this clears pending play intent.
  /// @return Success when pause is requested or already intended. Ended returns InvalidState. Failed and closed
  /// sessions return an error.
  /// @see Play(), Snapshot()
  [[nodiscard]] PlaybackResult<void> Pause();

  /// @brief Set the gain used to prepare subsequent device submissions.
  ///
  /// A prepared device submission carries the gain selected at preparation. The default gain is 1 and the
  /// setting survives Reset(). While muted, the session stores this gain for later use.
  /// @param volume Finite value in [0, 1]. Zero produces silence and one applies unity gain.
  /// @return Success when the preference is stored. An invalid value returns InvalidArgument. Failed and closed
  /// sessions return an error.
  /// @see SetMuted()
  [[nodiscard]] PlaybackResult<void> SetVolume(double volume);

  /// @brief Set whether subsequent device submissions are silent.
  ///
  /// Muting preserves the selected gain and muted media still advances the playhead. The setting defaults to
  /// false and survives Reset().
  /// @param muted True to prepare silent samples, false to use the stored gain.
  /// @return Success when the preference is stored. Failed and closed sessions return an error.
  /// @see SetVolume()
  [[nodiscard]] PlaybackResult<void> SetMuted(bool muted);

  /// @brief Seal audio input and allow the accepted media to drain.
  ///
  /// Play intent controls whether pending audio drains. Use Play() to drain a paused session. A sealed tail
  /// can start below the refill threshold. Ended becomes observable after the device confirms the final sample
  /// and pauses. An epoch with no pending media can end while paused.
  /// @return Success when sealed or already sealed, including after Ended. Starting or Resetting returns
  /// InvalidState. Failed and closed sessions return an error.
  /// @see Play(), Snapshot(), Reset()
  [[nodiscard]] PlaybackResult<void> FinishAudio();

  /// @brief Retire the current epoch and recreate the device asynchronously.
  ///
  /// The discard barrier immediately clears queued media, the clock, underruns, sealing and play intent.
  /// Device discard and reopening follow on the worker. Gain and mute preferences carry forward. Wait for
  /// readiness before retrying input. The next accepted frame supplies the new timeline anchor.
  /// @return Success when reset is accepted or already pending. Starting returns InvalidState. Failed and
  /// closed sessions return an error.
  /// @see TrySubmitAudio(), Snapshot()
  [[nodiscard]] PlaybackResult<void> Reset();

  /// @brief Read one coherent copy of the published transport and timing state.
  ///
  /// The position and buffered duration use the sample grid of the session. Values can change immediately
  /// after the call. Use the wakeup callback to schedule another read when progress is available.
  /// @return The current snapshot, including any operational error. A moved-from handle returns a default
  /// closed snapshot.
  /// @see PlaybackSnapshot, AudioPlaybackConfig::wakeup
  [[nodiscard]] PlaybackSnapshot Snapshot() const;

  /// @brief Retire the session immediately and schedule native discard and release.
  ///
  /// Safe to repeat or call from wakeup. Retirement ends admission of new input, controls and wakeups.
  /// A previously admitted wakeup may finish after return. Device samples already committed may still play
  /// until native stop takes effect. Observe cleanup_completed before retiring state used by wakeups.
  /// @see AudioPlaybackConfig::cleanup_completed
  void Close() noexcept;

private:
  explicit DecodedPlayback(std::shared_ptr<detail::PlaybackSession> session);
  std::shared_ptr<detail::PlaybackSession> session_;
};
} // namespace huxerui::media
