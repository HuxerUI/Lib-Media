#pragma once

/// @file
/// @brief Platform image/video selection and publication to the system photo library for HuxerUI.
///
/// Include <huxerui/media_library.h> and link HuxerUI::Media. Register Install() through the application's
/// application hooks before obtaining composition-owned handles with UseMediaPicker() or UseMediaLibrary().
/// Launch asynchronous operations through TaskScope from UI events or committed Lifecycle setup.
/// Android and iOS provide backends; other platforms report unavailable capabilities and Unsupported operations.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/task.h>

namespace huxerui::media {

namespace detail { class MediaLibraryState; }

/// @brief Portable categories for platform media selection and publication failures.
///
/// User dismissal is represented by a successful empty selection. Task cancellation suppresses result delivery;
/// neither is an error category. Platform failures are mapped to the most specific category available to the adapter.
enum class MediaLibraryErrorCode {
  /// The operation, media format, platform picker, or requested selection limit is unsupported.
  Unsupported,
  /// Access to selected content, the source file, or photo-library publication was denied.
  PermissionDenied,
  /// The adapter rejected the local source, for example a missing file or an invalid media representation.
  InvalidSource,
  /// The session or host cannot accept the request, or required application configuration is missing.
  Unavailable,
  /// Reading selected content, copying a source, or publishing through the system photo library failed.
  Io,
  /// The failure could not be mapped more specifically, including a malformed platform result.
  Unknown,
};

/// @brief A media-library failure with a portable category and diagnostic information.
struct MediaLibraryError {
  /// Portable category suitable for application error handling.
  MediaLibraryErrorCode code = MediaLibraryErrorCode::Unknown;
  /// Human-readable diagnostic text; do not parse it as a stable machine-readable identifier.
  std::string message;
  /// Library or SDK diagnostic identifier, such as "media/permission"; may be empty when no code is available.
  /// This is not necessarily an operating-system error number. Use code for portable error handling.
  std::string platform_code;
};

/// @brief Either a successful operation value or a MediaLibraryError.
///
/// Uses the SDK Result contract. Check Succeeded() before reading Value() or Error(); reading the wrong branch throws
/// std::logic_error. Accessors borrow the stored value; std::move(result).Value() allows ownership to be transferred.
/// Selection dismissal is a successful empty vector. Saving uses void for confirmed publication with no payload.
/// A canceled Task delivers no result; cancellation follows the SDK TaskScope contract.
/// @tparam T Owned success type: MediaLibraryCapabilities, std::vector<FileReference>, or void for these APIs.
template <class T> using MediaLibraryResult = Result<T, MediaLibraryError>;

/// @brief The kinds of media offered by the platform picker.
///
/// Filters select system photo-library content; they do not enable document selection or format conversion.
enum class MediaPickerFilter {
  /// Offer images only.
  Images,
  /// Offer videos only.
  Videos,
  /// Offer both images and videos in one selection request.
  /// The Android ACTION_PICK fallback first offers a choice of image or video gallery through the system chooser.
  ImagesAndVideos,
};

/// @brief Media filtering and maximum result count for one platform picker request.
///
/// Options are validated by MediaPicker::PickAsync(), not when this aggregate is constructed.
struct MediaPickerOptions {
  /// Allowed media kinds; defaults to both images and videos.
  MediaPickerFilter filter = MediaPickerFilter::ImagesAndVideos;
  /// Positive maximum number of selected items. One enables single selection; larger values enable multiple selection.
  /// Zero is invalid and does not mean unlimited. Limits above MediaLibraryCapabilities::max_selection_count return
  /// Unsupported; the adapter does not silently clamp the request or truncate a platform result that exceeds it.
  /// Android's legacy ACTION_PICK gallery fallback accepts one item only, regardless of private multi-select extras.
  std::uint32_t selection_limit = 1;
};

/// @brief A snapshot of platform selection and publication support, independent of permission authorization.
///
/// Obtain through either handle's CapabilitiesAsync(). Modifying a returned copy does not configure the backend.
/// Support can change, and a supported operation can still fail because of permissions, source content, or host state.
struct MediaLibraryCapabilities {
  /// Largest selection_limit accepted by this backend; zero means platform selection is unavailable.
  /// This is a request limit, not the number of available assets or a guarantee that the user selects that many.
  /// Android reports the system Photo Picker maximum, or one for an available ACTION_PICK gallery. A particular
  /// filter may still be unsupported when no installed gallery handles that media kind.
  std::uint32_t max_selection_count = 0;
  /// Whether platform publication is supported, independently of CanPick(). Required permission may still be denied.
  bool can_save = false;

  /// @brief Tests whether this snapshot supports platform image/video selection.
  /// @return True when max_selection_count is positive; false when it is zero. This derived query does not grant
  /// broad library access, reserve presentation, or guarantee that a subsequent selection succeeds.
  [[nodiscard]] bool CanPick() const noexcept { return max_selection_count > 0; }
};

/// @brief A UI-thread handle to platform media selection owned by the UseMediaPicker() composition lifetime.
///
/// Copies refer to the same session and original host window. Retaining a handle does not keep that session active
/// after unmount. Unmount cancels pending platform work; a waiting application Task that remains active receives
/// Unavailable, while a canceled Task receives no result. Requests started before attachment or after unmount fail.
///
/// Access and handle destruction belong on the owning UI thread; this is not a thread-safe interface. Run returned
/// Tasks on that UI execution through TaskScope. Platform presentation shares SDK coordination with FilePicker.
/// Android prefers the system Photo Picker and falls back to an available ACTION_PICK gallery for single selection.
/// iOS uses PHPicker. No document-picker fallback is implicit.
/// @see UseMediaPicker(), MediaLibrary
class MediaPicker final {
public:
  /// @brief Queries platform selection and publication support without presenting UI or requesting permission.
  ///
  /// Android prefers an available system Photo Picker on Android 13+, or Android 11/12 with R SDK extension 2+.
  /// Otherwise an installed ACTION_PICK gallery provides single selection; multiple selection is unsupported on
  /// that path because it cannot enforce a portable maximum. At least one image/video handler must be available;
  /// PickAsync() checks handlers for the requested filter. iOS uses the library's iOS 15 minimum.
  /// Query this snapshot when preparing UI; platform availability and host state may change before PickAsync() starts.
  /// @return A Task yielding a result containing MediaLibraryCapabilities, or MediaLibraryError on query failure.
  /// On unsupported desktop and Web platforms, an attached session returns a zero selection limit and can_save = false.
  /// @pre Call and run the Task on the owning UI thread after committed attachment.
  Task<MediaLibraryResult<MediaLibraryCapabilities>> CapabilitiesAsync() const;

  /// @brief Presents the platform picker and obtains access to selected images or videos.
  ///
  /// Selection requests no broad photo-library or storage-read permission. Completion may wait for remote/provider
  /// content to become readable. If any selected item cannot be materialized, the operation fails and releases partial
  /// results. Unsupported platform selection or limits return Unsupported rather than using a document picker.
  /// On Android's ACTION_PICK fallback, Images and Videos open the corresponding gallery directly; ImagesAndVideos
  /// uses the system chooser to select a media kind first and requires both handlers. Only selection_limit = 1 is
  /// accepted on that path.
  ///
  /// Closing the picker without a selection succeeds with an empty vector. Task cancellation suppresses
  /// delivery and requests platform cancellation; the picker UI may remain visible until the system closes it.
  /// A nonempty successful vector contains at most selection_limit ordinary file references. Partial results are
  /// never returned as success when reading a selected item fails.
  ///
  /// References retain access or owned temporary data after picker dismissal and session unmount. Android retains
  /// SDK URI access; iOS copies provider files into SDK-owned temporary storage before returning them. Neither backend
  /// promises permanent local paths, persistent asset identities, or access after process restart. Keep a reference
  /// alive while using its projected path; a path alone does not retain temporary data or access. Available name,
  /// size, and content-type metadata may vary. Use FileReference::OpenReadAsync() or ImportToAsync() for large videos
  /// instead of loading all bytes into memory.
  /// @param options Allowed media kinds and a positive maximum count; defaults to one image or video.
  /// @return A Task yielding a result with selected file references on success or MediaLibraryError on failure.
  /// Check Succeeded() before Value(). Only an empty successful vector means user dismissal; failure has no selection.
  /// @pre Call and run the Task on the owning UI thread from UI events or committed Lifecycle work.
  /// @throws std::invalid_argument If filter is invalid or selection_limit is zero, synchronously before returning
  /// the Task, including when the session is unavailable.
  /// @see CapabilitiesAsync(), MediaPickerOptions, UseMediaPicker()
  Task<MediaLibraryResult<std::vector<FileReference>>> PickAsync(MediaPickerOptions options = {}) const;

private:
  explicit MediaPicker(std::shared_ptr<detail::MediaLibraryState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::MediaLibraryState> state_;
  friend MediaPicker UseMediaPicker();
};

/// @brief A UI-thread handle to system photo-library publication owned by the UseMediaLibrary() composition lifetime.
///
/// Copies share a session bound to the original window. All access and handle destruction belong on its UI thread;
/// run returned Tasks through TaskScope on that execution. Before attachment or after unmount, operations return
/// Unavailable. Unmount cancels platform requests and settles surviving application Tasks with Unavailable;
/// Tasks canceled through their scope receive no result. Retaining a handle does not extend the active session.
///
/// Saving creates a new asset on every successful call and preserves the source. No destination picker, transcoding,
/// album management, or separate gallery-notification call is provided. Success means the system photo library confirmed
/// publication; it does not guarantee immediate refresh in another gallery application or return a persistent asset ID.
///
/// Android uses MediaStore on Android 10+ without storage permission. iOS requires a nonempty
/// NSPhotoLibraryAddUsageDescription in the application's Info.plist and requests Photos add-only authorization.
/// A missing description returns Unavailable before prompting; denial returns PermissionDenied. An authorization
/// prompt may outlive Task cancellation. Desktop and Web publication returns Unsupported.
///
/// Cancellation suppresses delivery but cannot undo committed publication. Android attempts to delete an unfinished
/// pending entry, subject to provider cleanup failures. A submitted iOS Photos transaction may still finish after
/// cancellation; its staged source remains retained until platform completion. Canceling or unmounting is not a
/// rollback guarantee.
/// @see UseMediaLibrary(), MediaPicker
class MediaLibrary final {
public:
  /// @brief Queries platform publication and selection support without requesting permission or accessing a source.
  ///
  /// can_save is independent of CanPick(). In particular, Android can support saving while its platform picker is absent.
  /// A true can_save reports platform support, not current authorization or support for every image/video format.
  /// @return A Task yielding a result containing MediaLibraryCapabilities, or MediaLibraryError on query failure.
  /// Unsupported desktop and Web platforms return a zero selection limit and can_save = false while attached.
  /// @pre Call and run the Task on the owning UI thread after committed attachment.
  Task<MediaLibraryResult<MediaLibraryCapabilities>> CapabilitiesAsync() const;

  /// @brief Publishes a local image as a new asset in the system photo library.
  ///
  /// Image formats are subject to platform validation and support. The source is copied, not moved or transcoded;
  /// saving the same source again creates another asset. iOS may present an add-only authorization prompt.
  /// @param source Application-accessible local image file. Keep it readable and unchanged until completion.
  /// File is a path value, not a retained access grant; import a FileReference to a local File first, or retain the
  /// reference for the entire operation if using its accessible projected path. Do not pass a content URI as a path.
  /// @return A Task yielding MediaLibraryResult<void>. Succeeded() is true only after confirmed publication;
  /// otherwise Error() describes the failure. Success carries no payload.
  /// No separate gallery scan is required. Cancellation delivers no result and may leave a committed asset.
  /// @pre Call and run the Task on the owning UI thread from UI events or committed Lifecycle work.
  /// @see MediaLibrary for permissions, lifecycle, source ownership, and publication cancellation semantics.
  Task<MediaLibraryResult<void>> SaveImageAsync(File source) const;

  /// @brief Publishes a local video as a new asset in the system photo library.
  ///
  /// The source must contain a video representation accepted by the platform backend. No codec conversion or editing
  /// is performed. Large sources are copied through file I/O without requiring an application-owned byte buffer.
  /// Repeated successful calls create separate assets and preserve the source. iOS may request add-only authorization.
  /// @param source Application-accessible local video file, kept readable and unchanged until completion.
  /// As with SaveImageAsync(), a File path does not retain a FileReference's access capability or temporary data.
  /// @return A Task yielding MediaLibraryResult<void>. Succeeded() is true only after confirmed publication;
  /// otherwise Error() describes the failure. Success carries no payload.
  /// Cancellation delivers no result; an already submitted or committed platform publication may still finish.
  /// @pre Call and run the Task on the owning UI thread from UI events or committed Lifecycle work.
  /// @see SaveImageAsync(), MediaLibrary
  Task<MediaLibraryResult<void>> SaveVideoAsync(File source) const;

private:
  explicit MediaLibrary(std::shared_ptr<detail::MediaLibraryState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::MediaLibraryState> state_;
  friend MediaLibrary UseMediaLibrary();
};

/// @brief Creates or retrieves a platform media-picker session retained by the current composition scope.
///
/// Attachment occurs through Lifecycle. Unmount cancels pending work and makes retained handles unavailable;
/// already returned FileReference values keep their own lifetime. Register Install() first and start operations
/// from UI events or committed Lifecycle work rather than as unconditional composition side effects.
/// @pre Call from an active HuxerUI composition scope with this library installed in its application.
/// @return A handle to the scope-owned picker session, bound to its original window.
/// @code{.cpp}
/// // Define this component in a codegen-enabled .cpp file.
/// #include <huxerui/huxerui.h>
/// #include <huxerui/media_library.h>
///
/// using namespace huxerui;
/// using namespace huxerui::media;
///
/// [[huxerui::composable]]
/// View PhotoPickerPane() {
///   auto picker = UseMediaPicker();
///   auto tasks = UseTaskScope();
///   auto files = UseState(std::make_shared<std::vector<FileReference>>());
///   auto message = UseState(std::string("No selection yet."));
///   return Column {
///     Button("Choose photos").OnClick([picker, tasks, files, message] {
///       tasks.Launch([picker, files, message]() -> Task<void> {
///         auto result = co_await picker.PickAsync({MediaPickerFilter::Images, 3});
///         if (!result.Succeeded()) {
///           message = result.Error().message;
///         } else {
///           auto selected = std::move(result).Value();
///           message = selected.empty() ? "Selection dismissed." : "Selected " +
///               std::to_string(selected.size()) + " photo(s).";
///           if (!selected.empty()) files = std::make_shared<std::vector<FileReference>>(std::move(selected));
///         }
///       });
///     }),
///     Text(message),
///   }.With(Spacing(8.0F));
/// }
/// @endcode
/// The example retains selected references in composition state; dismissal leaves the previous selection intact.
/// Query CapabilitiesAsync() when adapting controls or selection limits to the current device.
MediaPicker UseMediaPicker();

/// @brief Creates or retrieves a photo-library publication session retained by the current composition scope.
///
/// The session attaches through Lifecycle and becomes unavailable on unmount. It can save local files without a
/// MediaPicker, media player, or video view. Retaining the handle elsewhere does not extend its active lifetime.
/// @pre Call from an active HuxerUI composition scope with Install() registered in the application's hooks.
/// @return A handle to the scope-owned publication session, bound to its original window.
/// @code{.cpp}
/// // With the includes and namespaces from the UseMediaPicker() example:
/// [[huxerui::composable]]
/// View SaveImagePane(File source) {
///   auto library = UseMediaLibrary();
///   auto tasks = UseTaskScope();
///   auto message = UseState(std::string{});
///   return Column {
///     Button("Save image").OnClick([library, tasks, source, message] {
///       tasks.Launch([library, source, message]() -> Task<void> {
///         auto result = co_await library.SaveImageAsync(source);
///         if (!result.Succeeded()) message = result.Error().message;
///         else message = "Image saved.";
///       });
///     }),
///     Text(message),
///   }.With(Spacing(8.0F));
/// }
/// @endcode
/// The caller supplies an accessible image file and keeps its contents available until the save completes.
/// Use SaveVideoAsync() for a local video; the same ownership and permission rules apply.
MediaLibrary UseMediaLibrary();

/// @brief Registers this platform's playback, video output, and media-library integration for an application.
///
/// Add this function once to AppOptions::application_hooks for each application using the library. The declaration
/// in media_player.h names the same function; applications using both features still register it only once.
/// Do not call it during ordinary recomposition. Installation does not create sessions, present UI, or grant access.
/// @param context Application context receiving the library's platform registrations during installation.
/// @code{.cpp}
/// // App is the application's root View function.
/// const huxerui::Application application{
///     App,
///     {.application_hooks = {huxerui::media::Install}},
/// };
/// @endcode
void Install(ApplicationContext& context);

} // namespace huxerui::media
