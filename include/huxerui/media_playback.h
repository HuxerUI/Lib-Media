#pragma once

/// @file
/// @brief Playback and presentation timing for decoded audio and video frames.
///
/// Include <huxerui/media_playback.h> and link HuxerUI::Media. Create a session with a fixed PCM format and
/// submit AudioFrame values from a decoder or another producer. The session owns the output device and the
/// playback timeline. The producer owns source loading, decoding, seeking and any input format conversion.
///
/// PCM playback is available on Android API 26+, iOS 15+, macOS, Windows, and supported browsers. Availability depends on
/// the output route, PCM format, and browser capabilities.
/// Use the API independently of a View, Runtime or window. The application manages audio focus and background
/// playback policy. See AudioPlaybackConfig for supported formats and callback delivery. Enable video in
/// DecodedPlaybackConfig to add a caller-driven presenter on Android, iOS, macOS, Windows, or Web.

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <huxerui/media_format.h>

namespace huxerui::media {
namespace detail { class DecodedSession; }

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
  /// All configured tracks reached their presentation boundaries. Reset to start another epoch.
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
  /// its native worker or the owning browser event loop, outside session locks. Native delivery may race Create(),
  /// so capture independently owned or weak state. The callback may call Close(). Return promptly without
  /// throwing or waiting for cleanup.
  std::function<void()> wakeup;

  /// Optional final result delivered once after Close(), destruction or an operational failure.
  /// Delivery follows device release, the return of admitted wakeups and, on native platforms, worker exit.
  /// On Web, delivery follows asynchronous AudioContext close on the owning event loop. Success confirms release.
  /// Failure describes a release error independently of any operational error. Delivery remains pending while
  /// unsafe device work is still outstanding. The callback runs off the caller's stack.
  /// Use captures that survive retirement. Return promptly without throwing or waiting for other media work.
  std::function<void(PlaybackResult<void>)> cleanup_completed;
};

/// @brief Enabled tracks for one decoded playback session.
///
/// Enable at least one track. Audio is supported on Android API 26+, iOS 15+, macOS, Windows, and supported browsers.
/// Video is supported on Android, iOS, macOS, Windows, and Web. Submit both enabled tracks from the same timeline.
struct DecodedPlaybackConfig {
  /// PCM device configuration. Leave empty for video-only playback.
  std::optional<AudioPlaybackConfig> audio;
  /// Enable the bounded decoded video queue and caller-driven presentation.
  bool video = false;
};

/// @brief A consistent snapshot of the current epoch and its observable transport state.
struct PlaybackSnapshot {
  /// Last published transport state. A default snapshot describes a closed handle.
  PlaybackState state = PlaybackState::Closed;
  /// Playback position in the active timeline. PCM uses the configured sample rate.
  /// PCM starts at the first accepted frame's PTS and advances with device-confirmed media. It freezes when
  /// paused or starved and ignores device-inserted silence. Video-only position becomes available when
  /// PresentVideo() first observes a queued frame. Video-only playback and a video tail after confirmed audio
  /// drain use a nanosecond clock. Compare or rescale MediaTimestamp values when the clock changes.
  std::optional<MediaTimestamp> position;
  /// Duration of accepted media awaiting presentation, including samples submitted to the device.
  /// Expressed at the configured sample rate. Failed and closed sessions report zero queued duration.
  MediaTimestamp audio_buffered_duration;
  /// Number of starvation gaps confirmed after playback started in this epoch. Reset() clears the count.
  std::uint64_t audio_underruns = 0;
  /// True once FinishAudio() seals audio input for this epoch. Use state to observe overall drain completion.
  bool audio_input_finished = false;
  /// Operational failure while state is Failed. The cleanup callback carries any separate release error.
  std::optional<PlaybackError> error;
};

/// @brief Identifies one input and presentation generation of a DecodedPlayback session.
///
/// Capture this value before starting producer work and pass it with the resulting frames and EOS. Reset and Close
/// retire it immediately. Tokens from another session are invalid even if its playback position matches.
class PlaybackEpoch {
public:
  /// Creates an invalid token. Use DecodedPlayback::Epoch() to capture a live generation.
  PlaybackEpoch() = default;

  /// Compares the identity of two generations. A retired token remains comparable.
  bool operator==(const PlaybackEpoch&) const = default;

private:
  explicit PlaybackEpoch(std::shared_ptr<const void> identity) : identity_(std::move(identity)) {}
  std::shared_ptr<const void> identity_;
  friend class detail::DecodedSession;
};

/// @brief The work completed by one call to PresentVideo().
struct VideoPresentation {
  /// Present if a frame was published. Its value is the scheduling position minus that frame's PTS in milliseconds.
  std::optional<double> lag_ms;
  /// Number of queued frames discarded because their presentation intervals had elapsed.
  std::size_t dropped = 0;
};

/// @brief A move-only session for bounded decoded audio, video, or synchronized playback.
///
/// Submit contiguous PCM and video in presentation order. Retry the same frame after WouldBlock. For video,
/// drive PresentVideo() on the presentation thread. Seal each enabled track and use Snapshot() to observe Ended.
/// Reset() retires the timeline for another stream or seek.
///
/// On native platforms, controls, submission and snapshots may run concurrently on a live handle. Concurrent
/// submissions are ordered by acceptance. Move or destroy the handle after its callers stop. Each native session
/// serializes device work on its worker. On Web, use the session on the browser window event loop that created it.
/// Audio controls acquire locks and submission copies bytes. Call outside real-time render callbacks.
/// @see AudioPlaybackConfig, AudioFrame, PlaybackSnapshot
class DecodedPlayback final {
public:
  /// @brief Create a session for the enabled tracks and open any configured PCM device asynchronously.
  ///
  /// Video-only sessions begin in Paused. With audio, the initial state is Starting. Device open errors publish
  /// Failed and initiate cleanup. On native platforms, opening and wakeups can race Create(). Confirmed audio position
  /// drives synchronized video until audio drains. Video-only playback and a video tail after audio drain use
  /// monotonic time. Drive PresentVideo() periodically when video is enabled, including during drain.
  /// On iOS, configure and activate the application-owned AVAudioSession before enabling audio. An interruption,
  /// lost output route, or engine invalidation fails the current run even while paused. Restore the audio
  /// session before creating a fresh playback session.
  /// On Windows, audio uses the default render endpoint in WASAPI shared mode. Session disconnection fails
  /// the run and wakes the worker even while paused. Create a fresh session after restoring the output route.
  /// On Web, enabling audio requires a secure context with AudioWorklet and output timestamp support.
  /// @param config Enabled tracks and PCM settings, copied into the session.
  /// @return An owning handle, or a synchronous Unsupported, InvalidArgument or ResourceExhausted error.
  /// A synchronous failure schedules no callbacks.
  /// @see Snapshot(), Epoch(), PresentVideo()
  static PlaybackResult<DecodedPlayback> Create(const DecodedPlaybackConfig& config);

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
  /// @param epoch Current token from Epoch() or Reset(). Retired and foreign tokens return InvalidState.
  /// @param frame Valid frame in the configured format. Keep its plane bytes stable throughout the call.
  /// @return Accepted after copying the whole frame, after which the producer can release the source bytes.
  /// WouldBlock consumes no bytes during opening, reset or temporary saturation. Retry the same frame when
  /// the session can accept more. InvalidArgument covers malformed PCM, timestamp discontinuities and a frame
  /// larger than the entire capacity. Sealed input returns InvalidState. Failed and closed sessions report
  /// their operational error or Closed, respectively.
  /// @see AudioFrame::Validate(), AudioPlaybackConfig::buffer_capacity, FinishAudio(), Reset()
  PlaybackResult<SubmitStatus> TrySubmitAudio(const PlaybackEpoch& epoch, const AudioFrame& frame);

  /// @brief Retain a decoded video frame for presentation in the given generation.
  /// @param epoch Current token from Epoch(). Retired and foreign tokens return InvalidState.
  /// @param frame Valid CPU frame with a positive duration. Submit in nondecreasing PTS order and keep the
  /// underlying plane bytes immutable while retained. The session retains the plane references without copying pixels.
  /// A plane owner's final release may run during presentation or retirement. Keep it short and avoid reentering
  /// the same session from its destructor.
  /// @return Accepted after retaining the frame or WouldBlock while opening, resetting, or at the two-frame limit.
  /// Disabled or sealed video input returns InvalidState. Invalid frame geometry or timing returns InvalidArgument.
  PlaybackResult<SubmitStatus> TrySubmitVideo(const PlaybackEpoch& epoch, const VideoFrame& frame);

  /// @brief Seal video input for this generation and preserve the final queued frame through presentation.
  /// @return Success when sealed or already sealed. Disabled video and retired or foreign epochs return InvalidState.
  /// @see FinishAudio(), PresentVideo(), Snapshot()
  PlaybackResult<void> FinishVideo(const PlaybackEpoch& epoch);

  /// @brief Advance video scheduling and synchronously publish at most one due frame.
  ///
  /// Call from the thread that owns the renderer or texture publisher, including while audio is draining. The
  /// callback borrows the frame for this call and may read Snapshot(). Keep it short. Calls to PresentVideo(),
  /// Reset() and Close() serialize with an admitted callback. Call them after publish returns. A retired epoch
  /// cannot begin a publication. Exceptions from publish propagate and the selected frame remains pending.
  ///
  /// The presenter owns the texture. Pause, Ended, Reset and Close leave an already published image available.
  /// Replace or clear the presentation target on the presentation thread if a new epoch should start blank.
  /// @param epoch Current token from Epoch(). Retired and foreign tokens return InvalidState.
  /// @param publish Synchronous frame consumer, such as an upload to a caller-owned ExternalTexture.
  /// @return Publication and discard information. An empty lag means no frame was published.
  PlaybackResult<VideoPresentation> PresentVideo(
      const PlaybackEpoch& epoch, const std::function<void(const VideoFrame&)>& publish);

  /// @brief Request playback or record play intent while opening or resetting.
  ///
  /// Startup and refill wait for available PCM and the configured threshold. A sealed short tail or capacity
  /// pressure can start sooner. In a synchronized session, the audio device also waits until the first video
  /// frame is queued or video input is sealed. Resuming a paused device can continue with its pending media.
  /// On Web, request play from a user gesture such as a button click so browser autoplay policy can allow output.
  /// @return Success when play is requested or already intended. Inspect Snapshot() for the observed device
  /// state. Ended requires Reset() and returns InvalidState. Failed and closed sessions return an error.
  /// @see Pause(), Snapshot(), AudioPlaybackConfig::refill_threshold
  PlaybackResult<void> Play();

  /// @brief Clear play intent and request a pause while keeping queued media.
  ///
  /// Paused reports that the native pause took effect and the published position is frozen. Device progress
  /// already admitted may still advance before that boundary. During opening or reset, this clears pending play intent.
  /// A pending device start can be superseded by pause while retaining submitted audio.
  /// @return Success when pause is requested or already intended. Ended returns InvalidState. Failed and closed
  /// sessions return an error.
  /// @see Play(), Snapshot()
  PlaybackResult<void> Pause();

  /// @brief Set the gain used to prepare subsequent device submissions.
  ///
  /// A prepared device submission carries the gain selected at preparation. The default gain is 1 and the
  /// setting survives Reset(). While muted, the session stores this gain for later use.
  /// @param volume Finite value in [0, 1]. Zero produces silence and one applies unity gain.
  /// @return Success when the preference is stored. An invalid value returns InvalidArgument. Disabled audio
  /// returns InvalidState. Failed and closed sessions return an error.
  /// @see SetMuted()
  PlaybackResult<void> SetVolume(double volume);

  /// @brief Set whether subsequent device submissions are silent.
  ///
  /// Muting preserves the selected gain and muted media still advances the playhead. The setting defaults to
  /// false and survives Reset().
  /// @param muted True to prepare silent samples, false to use the stored gain.
  /// @return Success when the preference is stored. Disabled audio returns InvalidState. Failed and closed
  /// sessions return an error.
  /// @see SetVolume()
  PlaybackResult<void> SetMuted(bool muted);

  /// @brief Seal audio input and allow the accepted media to drain.
  ///
  /// Play intent controls whether pending audio drains. Use Play() to drain a paused session. A sealed tail
  /// can start below the refill threshold. In PCM-only sessions, Ended becomes observable after the device
  /// confirms the final sample and pauses. An epoch with no pending media can end while paused.
  /// @param epoch Current token from Epoch() or Reset(). Retired and foreign tokens return InvalidState.
  /// @return Success when sealed or already sealed, including after Ended. Starting or Resetting returns
  /// InvalidState. Failed and closed sessions return an error.
  /// @see Play(), Snapshot(), Reset()
  PlaybackResult<void> FinishAudio(const PlaybackEpoch& epoch);

  /// @brief Retire the current epoch and recreate the device asynchronously.
  ///
  /// The discard barrier waits for an admitted video upload, then clears queued media, the clock, underruns,
  /// sealing and play intent. Device discard and reopening follow on the worker or browser event loop. Video-only
  /// reset completes synchronously. Gain and mute preferences carry forward. Wait for audio readiness before retrying.
  /// The next accepted frame supplies the new timeline anchor.
  /// @return The new generation. A repeated reset during device recreation returns the pending generation.
  /// Starting returns InvalidState. Failed and closed sessions return an error.
  /// @see TrySubmitAudio(), Snapshot()
  PlaybackResult<PlaybackEpoch> Reset();

  /// @brief Capture the current input and publication generation without retaining the session.
  /// @return The current token, or an invalid token for a closed or moved-from handle.
  [[nodiscard]] PlaybackEpoch Epoch() const;

  /// @brief Read one coherent copy of the published transport and timing state.
  ///
  /// The buffered duration describes PCM on its sample grid. Values can change immediately
  /// after the call. Use the wakeup callback to schedule another read when progress is available.
  /// @return The current snapshot, including any operational error. A moved-from handle returns a default
  /// closed snapshot.
  /// @see PlaybackSnapshot, AudioPlaybackConfig::wakeup
  [[nodiscard]] PlaybackSnapshot Snapshot() const;

  /// @brief Retire the session and schedule native discard and release.
  ///
  /// Safe to repeat or call from wakeup. Close waits for an admitted video upload to return. Retirement then
  /// ends admission of new input, controls and wakeups.
  /// A previously admitted wakeup may finish after return. Device samples already committed may still play
  /// until native stop takes effect. Observe cleanup_completed before retiring state used by wakeups.
  /// @see AudioPlaybackConfig::cleanup_completed
  void Close() noexcept;

private:
  explicit DecodedPlayback(std::shared_ptr<detail::DecodedSession> session);
  std::shared_ptr<detail::DecodedSession> session_;
};
} // namespace huxerui::media
