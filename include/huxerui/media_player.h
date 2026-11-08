#pragma once

/// @file
/// @brief Platform audio/video playback, observable state, and an optional video view for HuxerUI.
///
/// Include <huxerui/media_player.h> and link HuxerUI::Media. Register Install() through the application's
/// application hooks before creating a session with UseMediaPlayer(). Applications own playback controls and platform
/// permissions; supported codecs and network protocols depend on the platform backend.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/file.h>
#include <huxerui/geometry.h>
#include <huxerui/lifecycle.h>
#include <huxerui/paint.h>
#include <huxerui/app.h>
#include <huxerui/view.h>

namespace huxerui::media {

namespace detail {
class PlayerState;
struct PlayerAccess;
} // namespace detail

/// @brief A media timeline position or duration expressed in double-precision seconds.
///
/// Fractional seconds are supported. Use count() to obtain seconds for display; this is not a wall-clock timestamp.
/// @code{.cpp}
/// const huxerui::media::MediaTime position{12.5};
/// const double seconds = position.count();
/// @endcode
using MediaTime = std::chrono::duration<double>;

/// @brief An HTTP or HTTPS media location and optional request headers.
///
/// Networking, redirects, and decoding are performed by the platform media backend. Applications remain responsible
/// for network permissions, transport-security policy, and browser CORS or mixed-content restrictions.
struct MediaHttpSource {
  /// Absolute HTTP or HTTPS URL with a nonempty authority; validated when wrapped in MediaSource.
  std::string url;
  /// Request headers. Names must be nonempty HTTP tokens; values must not contain NUL, CR, or LF.
  /// Malformed headers are rejected when wrapped in MediaSource. Nonempty headers currently produce an Unsupported
  /// playback error on every adapter when loaded; they are never silently dropped. Use an empty map for platform requests.
  std::map<std::string, std::string, std::less<>> headers;
};

/// @brief A retained local file, picker-granted file reference, or HTTP media input.
///
/// The loaded source is retained while the backend uses it. FileReference preserves the original access capability;
/// use it for picker results and browser files rather than extracting a path and losing access permissions.
/// Android uses the platform URI, Web uses an object URL, and desktop/iOS adapters require an accessible file location.
/// References without a backend-readable location report Unsupported. Raw byte streams are not media inputs here.
///
/// Wrapping an HTTP source validates its URL and header syntax and throws std::invalid_argument on invalid input.
/// It does not open or validate the media itself; asynchronous loading errors are reported by MediaPlayer::OnError().
/// @code{.cpp}
/// using namespace huxerui::media;
/// MediaSource local{huxerui::File("/absolute/path/to/video.mp4")};
/// MediaSource remote{MediaHttpSource{"https://example.com/video.mp4", {}}};
/// @endcode
class MediaSource final {
public:
  /// @brief Retains a local file path for playback without opening or inspecting its contents.
  /// @param file Local file value copied or moved into the source.
  explicit MediaSource(huxerui::File file);

  /// @brief Retains a file reference and its access capability for playback.
  /// @param reference File reference copied or moved into the source; platform loading checks its readability.
  explicit MediaSource(huxerui::FileReference reference);

  /// @brief Validates and retains an HTTP or HTTPS source without starting a request.
  /// @param source Absolute URL and request headers copied or moved into the source.
  /// @throws std::invalid_argument If the URL or header syntax violates MediaHttpSource's requirements.
  explicit MediaSource(MediaHttpSource source);

private:
  std::variant<huxerui::File, huxerui::FileReference, MediaHttpSource> value_;
  friend struct detail::PlayerAccess;
};

/// @brief The published playback lifecycle state, distinct from buffering and seeking flags.
enum class MediaPlayerStatus {
  /// No source is loaded; this is also the initial state and the state after Clear().
  Empty,
  /// The current source is being prepared. Play() can record intent to start when it becomes ready.
  Loading,
  /// The source is prepared; this does not imply that playback has started.
  Ready,
  /// The backend has reported playback. Check MediaPlayerState::is_buffering for buffering separately.
  Playing,
  /// The backend has reported a pause, or playback was denied by platform policy.
  Paused,
  /// A platform interruption prevents intended playback; resumption remains subject to platform policy.
  Interrupted,
  /// Natural completion of the current source. Successful looping does not enter this state.
  Ended,
  /// A fatal error stopped the current source. Load() can retry or replace it; Clear() releases it.
  Failed
};

/// @brief Whether the current source accepts timeline seeks according to the backend.
enum class MediaSeekability {
  /// The backend has not established seekability; SeekTo() is not accepted yet.
  Unknown,
  /// The backend reports that this source cannot seek.
  NotSeekable,
  /// The backend reports that this source can seek.
  Seekable
};

/// @brief Portable categories for asynchronous media failures.
///
/// Use MediaError::fatal to decide whether the source has failed. The category alone does not determine severity.
enum class MediaErrorCode {
  /// The requested media location could not be found.
  NotFound,
  /// Access to the source was denied.
  PermissionDenied,
  /// The backend could not use the supplied media source.
  InvalidSource,
  /// The requested source, format, or capability is unsupported by the adapter.
  Unsupported,
  /// A network operation needed to load or play the source failed.
  Network,
  /// Media parsing or decoding failed.
  Decode,
  /// An audio or video output operation failed.
  Output,
  /// Platform playback policy denied playback, for example a browser user-gesture requirement.
  PlaybackNotAllowed,
  /// A requested seek, including a loop restart, failed.
  SeekFailed,
  /// The backend error could not be mapped to a more specific category.
  Unknown,
};

/// @brief A buffered interval on the media timeline, expressed in seconds.
///
/// Endpoints describe backend-reported availability, not an exact frame boundary or a download guarantee.
struct MediaTimeRange {
  /// Beginning of the interval relative to the media timeline origin.
  MediaTime start{};
  /// End position of the interval, not its length; end is at or after start.
  MediaTime end{};

  /// @brief Compares both stored interval endpoints.
  /// @return True if both start values and both end values match, otherwise false.
  bool operator==(const MediaTimeRange&) const = default;
};

/// @brief The latest published position, optional duration, and optional buffered intervals.
///
/// These values come from the backend; they are not advanced by a UI clock. Query through MediaPlayer::Snapshot()
/// or subscribe with MediaPlayer::OnProgress(). Unknown metadata is distinct from a known zero value.
struct MediaProgress {
  /// Last published playback position. SeekTo() does not optimistically replace it with the requested position.
  MediaTime position{};
  /// Finite media duration, when known. Missing includes live streams without a finite duration; zero is a known value.
  std::optional<MediaTime> duration;
  /// Backend-reported buffered intervals. Missing means unavailable; an empty vector means no reported intervals.
  /// Do not assume the intervals cover the entire source or form one continuous range.
  std::optional<std::vector<MediaTimeRange>> buffered_ranges;

  /// @brief Compares the position, duration, and buffered intervals.
  /// @return True if all stored fields are equal, including optional-value presence and interval order.
  bool operator==(const MediaProgress&) const = default;
};

/// @brief A media failure with a portable category and optional platform diagnostic information.
struct MediaError {
  /// Portable category suitable for application error handling.
  MediaErrorCode code = MediaErrorCode::Unknown;
  /// Human-readable diagnostic text; do not parse it as a stable machine-readable identifier.
  std::string message;
  /// Backend-specific diagnostic code as text, or an empty string when no code is available.
  std::string platform_code;
  /// True when playback enters Failed and the error is retained in MediaPlayerState::error.
  /// False for recoverable notifications; these are delivered through OnError() without setting the retained error.
  bool fatal = false;

  /// @brief Compares the category, diagnostic text, platform code, and severity.
  /// @return True if all four stored fields are equal, otherwise false.
  bool operator==(const MediaError&) const = default;
};

/// @brief A value snapshot of one media session, including playback state, metadata, and user preferences.
///
/// Obtain it through MediaPlayer::Snapshot(). Modifying a returned copy does not control the player; use commands
/// such as Play(), SeekTo(), and SetVolume(). Metadata is reset when the source is replaced or cleared.
struct MediaPlayerState {
  /// Current published lifecycle state; use the buffering and seeking fields for those independent conditions.
  MediaPlayerStatus status = MediaPlayerStatus::Empty;
  /// Last published timeline information for the current source.
  MediaProgress progress;
  /// Current source's reported seek capability; Unknown and NotSeekable both prevent SeekTo().
  MediaSeekability seekability = MediaSeekability::Unknown;
  /// Requested playback intent, which can remain true during loading, buffering, or an interruption.
  /// This is not confirmation that media is currently playing.
  bool play_when_ready = false;
  /// Whether the backend currently reports buffering; not a replacement for status.
  bool is_buffering = false;
  /// Whether a seek is pending, including a coalesced request or an automatic loop restart.
  bool is_seeking = false;
  /// Whether the backend reports an audio track; missing means track information is not yet known.
  std::optional<bool> has_audio;
  /// Whether the backend reports a video track; missing means track information is not yet known.
  std::optional<bool> has_video;
  /// Backend-reported video dimensions when available, not the on-screen VideoSurface layout bounds.
  std::optional<huxerui::Size> video_size;
  /// Requested volume in [0, 1]. Muting does not change this value; Load() and Clear() preserve it.
  double volume = 1.0;
  /// Requested mute preference, preserved across Load() and Clear().
  bool muted = false;
  /// Requested loop preference, preserved across Load() and Clear(). Looping requires a finite, seekable source;
  /// a known nonseekable source reports a nonfatal Unsupported error without silently clearing this preference.
  bool looping = false;
  /// Only the current source's fatal error. Load() and Clear() reset it; recoverable errors are callback-only.
  std::optional<MediaError> error;

  /// @brief Compares all published state, metadata, and preference fields.
  /// @return True if all stored snapshot fields are equal, otherwise false.
  bool operator==(const MediaPlayerState&) const = default;
};

/// @brief A UI-thread handle to a session owned by the UseMediaPlayer() composition lifetime.
///
/// Handles referring to the same session share state, source, and preferences. Retaining a handle does not keep the
/// platform player active after its owning composition unmounts. Audio playback does not require a VideoSurface.
///
/// All access belongs on the owning UI thread; this is not a thread-safe control interface. State reads and commands
/// other than IsConnected() check thread ownership and throw std::logic_error on misuse.
/// Command bool results indicate acceptance, not asynchronous platform success. Disconnected commands return false
/// and are not queued. Observe Snapshot(), OnProgress(), OnEnded(), and OnError() for subsequent results.
/// @see UseMediaPlayer(), VideoSurface()
class MediaPlayer final {
public:
  /// @brief Tests whether the session is currently attached to its platform player.
  /// @return True while attached, regardless of whether a source is loaded or has failed; false before attachment
  /// or after the owning composition is unmounted. A false result is not a pending-command queue.
  /// @note Call on the owning UI thread even though this query does not enforce the thread check.
  [[nodiscard]] bool IsConnected() const noexcept;

  /// @brief Reads the latest published state without polling the backend synchronously.
  ///
  /// Reading during composition subscribes that scope to state changes. Read frequently changing progress in a small
  /// composable to limit recomposition. Outside composition, this is a point-in-time read on the owning UI thread.
  /// @return A value copy of the current state; disconnecting resets it to the default empty state.
  /// @throws std::logic_error If called from a different thread.
  [[nodiscard]] MediaPlayerState Snapshot() const;

  /// @brief Replaces the current source and begins asynchronous preparation without autoplay.
  ///
  /// Invalidates old-source callbacks, cancels pending seeks, clears playback intent and metadata, and enters Loading.
  /// Volume, mute, and looping preferences survive. Call Play() to request playback, including while loading;
  /// browser policy may instead require a user gesture after the source is ready.
  /// @param source Media input to retain until replaced, cleared, or disconnected.
  /// @return True if the connected session accepted the source; false if disconnected. True does not mean decoding
  /// or network access succeeded; use OnError() and Snapshot() to observe preparation.
  /// @throws std::logic_error If called from a different thread.
  /// @see MediaSource, Play(), Clear()
  bool Load(MediaSource source) const;

  /// @brief Releases the source, cancels pending seeks and playback intent, and returns to Empty.
  ///
  /// Invalidates queued old-source callbacks and clears the retained fatal error. Volume, mute, and looping are
  /// preserved. Clearing an already empty, connected session is successful and does not emit OnEnded().
  /// @return True if connected, including an already empty session; false if disconnected.
  /// @throws std::logic_error If called from a different thread.
  bool Clear() const;

  /// @brief Requests playback or records playback intent while the source is loading.
  ///
  /// Repeated requests are idempotent. From Ended, playback seeks to the beginning before restarting, requiring a
  /// finite duration and known seekability. During a seek, playback resumes after seek completion if intent remains set.
  /// Platform notifications determine Playing; interruption and browser policy may still prevent playback.
  /// @return True if accepted or already requested; false if disconnected, Empty, Failed, or unable to replay Ended.
  /// @throws std::logic_error If called from a different thread.
  bool Play() const;

  /// @brief Cancels playback intent and requests a pause without clearing the source or position.
  ///
  /// During loading this only cancels the pending play intent. It does not cancel an in-flight seek. Calling Pause()
  /// when play intent is already false succeeds without issuing another platform pause.
  /// @return True if accepted or already paused in intent; false if disconnected, Empty, or Failed.
  /// @throws std::logic_error If called from a different thread.
  bool Pause() const;

  /// @brief Requests a seek on a prepared source with known seekability.
  ///
  /// At most one platform seek runs at a time; later requests replace the pending target with the latest value.
  /// The published position is updated from platform completion, not optimistically from the request. A paused player
  /// stays paused unless Play() is requested; seeking from Ended leaves that state after completion.
  /// @param position Finite, nonnegative seconds from the media timeline origin; clamped to a known duration.
  /// @return True if accepted; false if disconnected, Loading, Failed, or not known to be seekable.
  /// @throws std::invalid_argument If position is negative or nonfinite, even when disconnected.
  /// @throws std::logic_error If called from a different thread.
  /// The example uses an application-owned State<std::string> named notice for feedback.
  /// @code{.cpp}
  /// return huxerui::Button("Seek to 30 seconds").OnClick([player, notice] {
  ///   if (!player.SeekTo(huxerui::media::MediaTime{30.0})) notice = "Seeking is unavailable.";
  /// });
  /// @endcode
  bool SeekTo(MediaTime position) const;

  /// @brief Sets the requested automatic replay preference, preserved across source changes.
  ///
  /// Successful loops require a finite, seekable source and do not emit OnEnded(). Known nonseekable sources report
  /// a nonfatal Unsupported error; failed loop seeks report SeekFailed and allow natural completion.
  /// @param looping True to request replay from the beginning at natural completion; false to disable future loops.
  /// @return True if the preference was accepted; false if disconnected. True does not guarantee loop support.
  /// @throws std::logic_error If called from a different thread.
  bool SetLooping(bool looping) const;

  /// @brief Sets the requested playback volume without changing the mute preference.
  /// @param volume Finite normalized volume in [0, 1], where 0 is silent and 1 is full player volume.
  /// @return True if connected and accepted; false if disconnected. The value survives Load() and Clear().
  /// @throws std::invalid_argument If volume is nonfinite or outside [0, 1], even when disconnected.
  /// @throws std::logic_error If called from a different thread.
  bool SetVolume(double volume) const;

  /// @brief Sets muting without losing the requested volume.
  /// @param muted True to silence output; false to restore output at the stored volume.
  /// @return True if connected and accepted; false if disconnected. The preference survives Load() and Clear().
  /// @throws std::logic_error If called from a different thread.
  bool SetMuted(bool muted) const;

  /// @brief Subscribes to progress for the declaring composition lifetime.
  ///
  /// Delivers an initial snapshot when connected, then progress from platform polling about every 250 ms and significant
  /// transitions such as load, clear, pause, seek completion, and end. Unchanged polling need not emit a callback;
  /// queued bursts may coalesce. This is neither a precise timer nor a video-frame callback.
  ///
  /// Multiple subscribers are independent. Unmounting removes this subscription; dependency changes replace it
  /// without recreating the player or loading its source again. Capture stable State handles, or pass changing
  /// captured values as dependencies so the installed callback is refreshed. Old-source notifications are discarded.
  /// @tparam Dependencies Types accepted by HuxerUI Lifecycle dependency tracking.
  /// @param handler Nonempty callback invoked on the owning UI thread with the current published progress.
  /// The referenced value is valid only for that invocation; copy it to retain it.
  /// @param dependencies Optional values or observable State dependencies controlling subscription replacement.
  /// @pre Call during composition, not from a click handler or background thread.
  /// @throws std::invalid_argument If handler is empty.
  /// @code{.cpp}
  /// auto progress = huxerui::UseState(huxerui::media::MediaProgress{});
  /// player.OnProgress([progress](const huxerui::media::MediaProgress& value) {
  ///   progress = value;
  /// });
  /// @endcode
  template <class... Dependencies>
  void OnProgress(std::function<void(const MediaProgress&)> handler, Dependencies&&... dependencies) const {
    if (!handler)
      throw std::invalid_argument("HuxerUI media progress handler must not be empty");
    Lifecycle([player = *this, handler = std::move(handler)] { return player.ConnectProgress(handler); }, *this,
              std::forward<Dependencies>(dependencies)...);
  }

  /// @brief Subscribes to natural completion, excluding successful loops, clearing, replacement, and fatal failure.
  ///
  /// The session enters Ended before the notification is delivered. Earlier completions are not replayed to a new
  /// subscriber, and queued notifications for a replaced source are discarded. A failed loop restart can report
  /// a recoverable error and then natural completion. Subscriptions are independent and removed on unmount.
  /// @tparam Dependencies Types accepted by HuxerUI Lifecycle dependency tracking.
  /// @param handler Nonempty callback invoked on the owning UI thread when the current source finishes naturally.
  /// @param dependencies Optional values or observable State dependencies that replace this subscription when changed.
  /// @pre Call during composition. Capture stable handles or include changing captures in dependencies.
  /// @throws std::invalid_argument If handler is empty.
  /// @code{.cpp}
  /// auto finished = huxerui::UseState(false);
  /// player.OnEnded([finished] { finished = true; });
  /// @endcode
  template <class... Dependencies> void OnEnded(std::function<void()> handler, Dependencies&&... dependencies) const {
    if (!handler)
      throw std::invalid_argument("HuxerUI media ended handler must not be empty");
    Lifecycle([player = *this, handler = std::move(handler)] { return player.ConnectEnded(handler); }, *this,
              std::forward<Dependencies>(dependencies)...);
  }

  /// @brief Subscribes to new fatal and recoverable errors without replaying historical failures.
  ///
  /// A fatal error enters Failed and is retained in Snapshot().error before delivery. Recoverable errors are delivered
  /// only through callbacks; they do not populate that field. Load() retries a failed source. Notifications belonging
  /// to a replaced source or disconnected session are discarded. Each subscription is removed on unmount.
  /// @tparam Dependencies Types accepted by HuxerUI Lifecycle dependency tracking.
  /// @param handler Nonempty callback invoked on the owning UI thread. Its error reference is valid only during the
  /// invocation; copy the value or its fields when retaining diagnostics.
  /// @param dependencies Optional values or observable State dependencies that replace this subscription when changed.
  /// @pre Call during composition. Capture stable handles or include changing captures in dependencies.
  /// @throws std::invalid_argument If handler is empty.
  /// @code{.cpp}
  /// auto message = huxerui::UseState(std::string{});
  /// player.OnError([message](const huxerui::media::MediaError& error) {
  ///   message = error.message;
  /// });
  /// @endcode
  template <class... Dependencies>
  void OnError(std::function<void(const MediaError&)> handler, Dependencies&&... dependencies) const {
    if (!handler)
      throw std::invalid_argument("HuxerUI media error handler must not be empty");
    Lifecycle([player = *this, handler = std::move(handler)] { return player.ConnectError(handler); }, *this,
              std::forward<Dependencies>(dependencies)...);
  }

  /// @brief Compares the retained session identity of two handles.
  /// @return True if both handles retain the same session state, otherwise false.
  bool operator==(const MediaPlayer&) const = default;

private:
  explicit MediaPlayer(std::shared_ptr<detail::PlayerState> state) : state_(std::move(state)) {}
  std::function<void()> ConnectProgress(std::function<void(const MediaProgress&)> handler) const;
  std::function<void()> ConnectEnded(std::function<void()> handler) const;
  std::function<void()> ConnectError(std::function<void(const MediaError&)> handler) const;
  std::shared_ptr<detail::PlayerState> state_;
  friend struct detail::PlayerAccess;
};

/// @brief Creates or retrieves a media session retained by the current composition scope.
///
/// The platform player attaches through Lifecycle and is cleared and disconnected when the owning scope unmounts.
/// Retaining a handle elsewhere does not extend that active lifetime. Callback subscriptions do not create players.
/// Register Install() first; issue playback commands from UI events or mounted lifecycle work rather than as
/// unconditional composition side effects. A newly declared session may not yet be connected during composition.
/// @pre Call from an active HuxerUI composition scope with this library installed in its root.
/// @return A handle to the scope-owned session, initially with no loaded source and default preferences.
/// @code{.cpp}
/// // Define this component in a codegen-enabled .cpp file.
/// #include <huxerui/huxerui.h>
/// #include <huxerui/media_player.h>
///
/// using namespace huxerui;
/// using namespace huxerui::media;
///
/// [[huxerui::composable]]
/// View PlayerPane() {
///   auto player = UseMediaPlayer();
///   auto notice = UseState(std::string{});
///   return Column {
///     VideoSurface(player).With(Frame{.height = 240.0F}),
///     Row {
///       Button("Load").OnClick([player, notice] {
///         if (!player.Load(MediaSource(File("/absolute/path/to/video.mp4")))) notice = "The player is unavailable.";
///       }),
///       Button("Play").OnClick([player, notice] {
///         if (!player.Play()) notice = "Playback cannot start for the current media.";
///       }),
///       Button("Pause").OnClick([player, notice] {
///         if (!player.Pause()) notice = "Playback cannot be paused right now.";
///       }),
///     }.With(Spacing(8.0F)),
///     Text(notice),
///   }.With(CrossAlign(CrossAxisAlignment::Stretch));
/// }
/// @endcode
/// The local path is illustrative; use an accessible file or a picker-provided FileReference on the target platform.
MediaPlayer UseMediaPlayer();

/// @brief Video fitting and alignment within a bounded VideoSurface.
///
/// These properties affect presentation only, not decoding, playback position, volume, or session ownership.
struct VideoSurfaceProperties {
  /// Contain fits the whole video, Cover fills with cropping, Fill stretches, None keeps its natural size, and
  /// ScaleDown only reduces oversized content. The default is Contain.
  huxerui::ImageFit fit = huxerui::ImageFit::Contain;
  /// Horizontal content placement: Start, Center, or End. Stretch is invalid; Center is the default.
  huxerui::HorizontalAlignment horizontal_alignment = huxerui::HorizontalAlignment::Center;
  /// Vertical content placement: Start, Center, or End. Stretch is invalid; Center is the default.
  huxerui::VerticalAlignment vertical_alignment = huxerui::VerticalAlignment::Center;

  /// @brief Compares video fitting and both content alignments.
  /// @return True if all three presentation properties are equal, otherwise false.
  bool operator==(const VideoSurfaceProperties&) const = default;
};

/// @brief Declares a video-only view bound to an existing media session.
///
/// Supply bounded layout; the view has no portable intrinsic size, playback controls, autoplay, or fullscreen behavior.
/// At most one surface can be mounted for a player. Removing only the surface neither pauses playback nor releases
/// the source, so audio can continue; unmounting the scope that owns UseMediaPlayer() still disconnects the session.
/// Changing properties updates presentation without loading the source again. Composition limits vary by platform.
/// @param player Session whose video output is displayed; it must belong to the same owning UI thread.
/// @param properties Fit and content alignment; defaults to centered Contain.
/// @return A declarative View to place in the HuxerUI layout.
/// @throws std::invalid_argument If either content alignment is Stretch.
/// @note Mounting a second surface for the same player is rejected with std::logic_error during binding.
/// @code{.cpp}
/// using namespace huxerui;
/// return media::VideoSurface(player, {
///     .fit = ImageFit::Cover,
///     .horizontal_alignment = HorizontalAlignment::Center,
///     .vertical_alignment = VerticalAlignment::Start,
/// }).With(Frame{.width = 320.0F, .height = 180.0F});
/// @endcode
huxerui::View VideoSurface(MediaPlayer player, VideoSurfaceProperties properties = {});

/// @brief Registers this platform's media player and video output integration for an application.
///
/// Add this function once to AppOptions::application_hooks for each application that uses the library. Do not call it
/// during ordinary recomposition. Installation does not create a playback session, load media, or grant permissions.
/// @param context Application context receiving the library's platform registrations during installation.
/// @code{.cpp}
/// // App is the application's root View function.
/// const huxerui::Application application{
///     App,
///     {.application_hooks = {huxerui::media::Install}},
/// };
/// @endcode
void Install(huxerui::ApplicationContext& context);

} // namespace huxerui::media
