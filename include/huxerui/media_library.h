#pragma once

/// @file
/// @brief System photo-library browsing, image/video selection, and publication for HuxerUI.
///
/// Include <huxerui/media_library.h> and link HuxerUI::Media. Register Install() through the application's
/// application hooks before obtaining a composition-owned handle with UseMediaLibrary().
/// Launch asynchronous operations through TaskScope from UI events or committed Lifecycle setup.
/// Android and iOS provide backends; other platforms report unavailable capabilities and Unsupported operations.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/external_texture.h>
#include <huxerui/lifecycle.h>
#include <huxerui/navigation.h>
#include <huxerui/task.h>
#include <huxerui/theme.h>
#include <huxerui/view.h>

namespace huxerui::media {

namespace detail { class MediaLibraryState; struct MediaLibraryAccess; }

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
  /// The requested asset or album is no longer visible to the application.
  NotFound,
  /// The page cursor no longer belongs to the current library revision. Restart from the first page.
  StaleCursor,
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
/// @tparam T Owned success type of the operation.
template <class T> using MediaLibraryResult = Result<T, MediaLibraryError>;

/// @brief The kinds of media to offer in a photo-library query or picker.
///
/// Filters select system photo-library content; they do not enable document selection or format conversion.
enum class MediaFilter {
  /// Offer images only.
  Images,
  /// Offer videos only.
  Videos,
  /// Offer both images and videos in one selection request.
  /// The Android ACTION_PICK fallback first offers a choice of image or video gallery through the system chooser.
  ImagesAndVideos,
};

/// @brief Media filtering and maximum result count for one system picker request.
///
/// Options are validated by MediaLibrary::PickAsync(), not when this aggregate is constructed.
struct SystemMediaPickerOptions {
  /// Allowed media kinds; defaults to both images and videos.
  MediaFilter filter = MediaFilter::ImagesAndVideos;
  /// Positive maximum number of selected items. One enables single selection; larger values enable multiple selection.
  /// Zero is invalid and does not mean unlimited. Limits above MediaLibraryCapabilities::max_selection_count return
  /// Unsupported; the adapter does not silently clamp the request or truncate a platform result that exceeds it.
  /// Android's legacy ACTION_PICK gallery fallback accepts one item only, regardless of private multi-select extras.
  std::uint32_t selection_limit = 1;
};

/// @brief A snapshot of platform browsing, selection, and publication support, independent of authorization.
///
/// Obtain through MediaLibrary::CapabilitiesAsync(). Modifying a returned copy does not configure the backend.
/// Support can change, and a supported operation can still fail because of permissions, source content, or host state.
struct MediaLibraryCapabilities {
  /// Largest selection_limit accepted by this backend; zero means platform selection is unavailable.
  /// This is a request limit, not the number of available assets or a guarantee that the user selects that many.
  /// Android reports the system Photo Picker maximum, or one for an available ACTION_PICK gallery. A particular
  /// filter may still be unsupported when no installed gallery handles that media kind.
  std::uint32_t max_selection_count = 0;
  /// Whether platform publication is supported, independently of CanPick(). Required permission may still be denied.
  bool can_save = false;
  /// Whether library browsing is supported. The host must separately obtain photo/video read access.
  bool can_browse = false;

  /// @brief Tests whether this snapshot supports platform image/video selection.
  /// @return True when max_selection_count is positive; false when it is zero. This derived query does not grant
  /// broad library access, reserve presentation, or guarantee that a subsequent selection succeeds.
  [[nodiscard]] bool CanPick() const noexcept { return max_selection_count > 0; }
};

/// Identifies the primary media representation of a library asset.
enum class MediaAssetKind { Image, Video };

/// Opaque identity of an asset in the current system library. Copies compare by value across albums.
/// Use IDs obtained from library queries. Their storage format and persistence across app launches are unspecified.
struct MediaAssetId {
  std::string value;
  bool operator==(const MediaAssetId&) const = default;
};

/// Opaque identity of a platform album. Albums with the same display name can have distinct IDs.
struct MediaAlbumId {
  std::string value;
  bool operator==(const MediaAlbumId&) const = default;
};

/// Metadata for a visible image or video. A value neither retains read permission nor opens the media file.
struct MediaAsset {
  MediaAssetId id;
  MediaAssetKind kind = MediaAssetKind::Image;
  /// Capture or creation time where known. Android may use the time the asset entered the library.
  std::optional<std::chrono::system_clock::time_point> date;
  /// Video playback duration when known. Images have no duration.
  std::optional<std::chrono::milliseconds> duration;
};

/// A visible album with at least one asset matching the requested filter.
struct MediaAlbum {
  MediaAlbumId id;
  std::string name;
  /// Newest matching asset, suitable for a thumbnail request.
  MediaAssetId cover_id;
};

/// Selects a page of metadata from a photo library. A missing album queries Recents across visible albums.
struct MediaAssetQuery {
  MediaFilter filter = MediaFilter::ImagesAndVideos;
  /// Album ID returned by ListAlbumsAsync(). Leave unset to query Recents.
  std::optional<MediaAlbumId> album_id;
  /// Number of records to request, in [1, 256].
  std::uint32_t page_size = 60;
  /// Opaque continuation from a previous page in this session with the same filter, album, and page size.
  std::optional<std::string> cursor;
};

/// A page ordered newest first, with an empty continuation at the end.
struct MediaAssetPage {
  std::vector<MediaAsset> assets;
  std::optional<std::string> next_cursor;
};

/// Requests a bounded photo or video poster at physical pixel dimensions while preserving aspect ratio.
struct MediaImageRequest {
  /// Positive pixel limits in [1, 4096]. The output may be smaller.
  std::uint32_t max_width = 0;
  std::uint32_t max_height = 0;
  /// Allow a platform provider to download the representation if needed.
  bool allow_network = false;
};

/// @brief A UI-thread handle to system photo-library browsing, selection, and publication.
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
/// @see UseMediaLibrary(), MediaLibraryPicker()
class MediaLibrary final {
public:
  /// @brief Queries platform browsing, publication, and selection support without requesting permission.
  ///
  /// can_save is independent of CanPick(). In particular, Android can support saving while its platform picker is absent.
  /// A true can_save reports platform support, not current authorization or support for every image/video format.
  /// @return A Task yielding a result containing MediaLibraryCapabilities, or MediaLibraryError on query failure.
  /// On desktop and Web, the attached session reports a zero selection limit and false for can_save and can_browse.
  /// @pre Call and run the Task on the owning UI thread after committed attachment.
  Task<MediaLibraryResult<MediaLibraryCapabilities>> CapabilitiesAsync() const;

  /// @brief Presents the platform picker and obtains access to selected images or videos.
  ///
  /// Android prefers the system Photo Picker on Android 13+, or Android 11/12 with R SDK extension 2+ when available.
  /// Otherwise, it uses an installed ACTION_PICK gallery. iOS uses PHPicker with the library's iOS 15 minimum.
  /// Platform presentation shares the SDK's coordination with FilePicker.
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
  /// @see CapabilitiesAsync(), SystemMediaPickerOptions, UseMediaLibrary()
  Task<MediaLibraryResult<std::vector<FileReference>>> PickAsync(SystemMediaPickerOptions options = {}) const;

  /// Lists matching albums and their cover IDs. Query Recents with an unset MediaAssetQuery::album_id.
  /// Call after attachment on the owning UI thread. The operation uses current read authorization without prompting.
  Task<MediaLibraryResult<std::vector<MediaAlbum>>> ListAlbumsAsync(
      MediaFilter filter = MediaFilter::ImagesAndVideos) const;

  /// Lists a page of visible asset metadata without opening files or loading thumbnails.
  /// Unknown dates sort last. Pagination is stable within a library revision, including ties.
  /// Use a returned cursor unchanged for the next page. On StaleCursor, discard the continuation and query anew.
  /// An unset album_id means Recents. An album with no remaining visible matches returns NotFound.
  Task<MediaLibraryResult<MediaAssetPage>> ListAssetsAsync(MediaAssetQuery query = {}) const;

  /// Looks up IDs across visible albums and returns the visible subset in input order, including duplicate inputs.
  /// A successful empty vector means no requested ID was visible. A provider failure is returned separately.
  Task<MediaLibraryResult<std::vector<MediaAsset>>> FindAssetsAsync(std::vector<MediaAssetId> ids) const;

  /// Loads a displayable photo or video poster. Pass the returned texture directly to huxerui::Image.
  /// Successful null means the asset is visible but this request has no representation. The texture retains
  /// its image independently of this handle. LoadImageAsync() does not return the source file or change the asset.
  /// The optional progress callback receives normalized values on the owning UI thread when the provider reports them.
  Task<MediaLibraryResult<std::shared_ptr<ExternalTexture>>> LoadImageAsync(
      MediaAssetId id, MediaImageRequest request, std::function<void(double)> on_progress = {}) const;

  /// Opens the current file representation for a visible asset. Android URI references remain subject to system
  /// authorization. iOS may copy into temporary storage retained by the returned reference. Keep the reference
  /// alive while using any projected path. This operation may download cloud assets and reports progress when available.
  Task<MediaLibraryResult<FileReference>> OpenAssetAsync(
      MediaAssetId id, std::function<void(double)> on_progress = {}) const;

  /// Subscribes during composition to library invalidation on the owning UI thread.
  /// Notifications may coalesce and carry no change list. Query again to refresh displayed data. No initial event
  /// is replayed. Changing dependencies replaces this subscriber, and unmount stops delivery.
  /// @throws std::invalid_argument If handler is empty.
  template <class... Dependencies>
  void OnChanged(std::function<void()> handler, Dependencies&&... dependencies) const {
    if (!handler) throw std::invalid_argument("Media library change handler must not be empty");
    Lifecycle([library = *this, handler = std::move(handler)] { return library.ConnectChanged(handler); },
              *this, std::forward<Dependencies>(dependencies)...);
  }

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

  bool operator==(const MediaLibrary&) const = default;

private:
  std::function<void()> ConnectChanged(std::function<void()> handler) const;
  explicit MediaLibrary(std::shared_ptr<detail::MediaLibraryState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::MediaLibraryState> state_;
  friend MediaLibrary UseMediaLibrary();
  friend struct detail::MediaLibraryAccess;
};

/// @brief Creates or retrieves a photo-library session retained by the current composition scope.
///
/// Attachment occurs through Lifecycle. Unmount cancels pending work and makes retained handles unavailable;
/// already returned FileReference values keep their own lifetime. Register Install() first and start operations
/// from UI events or committed Lifecycle work rather than as unconditional composition side effects.
/// @pre Call from an active HuxerUI composition scope with this library installed in its application.
/// @return A handle for system selection, browsing, and publication, bound to its original window.
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
///   auto library = UseMediaLibrary();
///   auto tasks = UseTaskScope();
///   auto files = UseState(std::make_shared<std::vector<FileReference>>());
///   auto message = UseState(std::string("No selection yet."));
///   return Column {
///     Button("Choose photos").OnClick([library, tasks, files, message] {
///       tasks.Launch([library, files, message]() -> Task<void> {
///         auto result = co_await library.PickAsync(SystemMediaPickerOptions{MediaFilter::Images, 3});
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
///
/// For publication, keep the source readable until the operation completes.
/// @code{.cpp}
/// // With the same includes and namespaces:
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

/// Optional local overrides for the in-app picker. Unset values follow the surrounding HuxerUI theme.
/// Each supplied component style is used in full. To give the picker a separate light or dark theme,
/// place it inside the corresponding HuxerUI Theme.
struct MediaLibraryPickerStyle {
  /// Top app bar style shared by the grid and preview.
  std::optional<TopAppBarStyle> top_app_bar;
  /// Style of the confirmation button, including its disabled appearance and indication.
  std::optional<ButtonStyle> confirm_button;
  /// Style of auxiliary buttons, including authorization, management, and retry.
  std::optional<ButtonStyle> secondary_button;
  /// Outline of selected grid items. Defaults to the resolved badge background.
  std::optional<Color> selection_outline;
  /// Fill of the selected badge in the grid and preview. Defaults to the theme's primary color.
  std::optional<Color> selection_badge_background;
  /// Color of the badge checkmark and selection number. Defaults to the theme's on-primary color.
  std::optional<Color> selection_badge_foreground;
  /// Fixed number of grid columns, when set. Defaults to three in a compact viewport and five elsewhere.
  std::optional<std::uint32_t> grid_columns;
  /// Requested gap between grid cells in logical pixels, in both directions. Narrow grids may reduce the gap.
  float grid_spacing = 3.0F;

  /// Returns the inherited appearance with no local overrides.
  static MediaLibraryPickerStyle Default() { return {}; }
  bool operator==(const MediaLibraryPickerStyle&) const = default;
};

/// Options for the in-app picker. The custom selection limit is independent of the system picker's limit.
struct MediaLibraryPickerOptions {
  MediaFilter filter = MediaFilter::ImagesAndVideos;
  /// Positive maximum. One selects a single asset; larger values allow ordered multiple selection.
  std::uint32_t selection_limit = 1;
  /// Browsing title. Defaults to the localized title for the active filter. Preview shows its own title.
  std::optional<StringVariant> title;
  /// Local appearance overrides. Updating the title or style preserves the current picker session.
  MediaLibraryPickerStyle style{};
  /// Receives all readable references in selection order after the user confirms.
  std::function<void(std::vector<FileReference>)> on_confirm;
  /// Invoked when the user exits the picker without submitting a selection.
  std::function<void()> on_cancel;
};

/// @brief Builds a full-height, composition-owned photo-library picker for Android and iOS.
///
/// Mount in a bounded viewport, such as a screen or navigation destination, after registering Install().
/// The view starts in Recents, paginates as the user scrolls, and keeps a shared selection across album and preview
/// changes. Tapping a thumbnail opens preview; the separate selection control changes selection. For multiple selection,
/// the selected-items strip opens the chosen assets in selection order. The album list opens below the top bar and
/// spans the picker width. The grid uses square cells sized to the available width and configured column count.
/// Preview supports horizontal paging, pinch zoom and pan for images, and playback for the current video. Returning
/// from preview restores the grid position. The picker inherits the host HuxerUI theme, locale, and motion policy.
/// Read authorization is requested only from an explicit user action.
/// A finished selection invokes on_confirm once. Exiting invokes on_cancel once. Unmount cancels outstanding work and
/// delivers neither callback. The host owns presentation and dismissal. Mount a new picker for another session.
/// @param options Filter, positive selection limit, optional title, local styles, and owning-thread completion callbacks.
/// A new filter or limit starts a fresh selection session.
/// @throws std::invalid_argument If the filter is invalid, the selection limit or specified column count is zero,
/// or grid spacing is negative or non-finite.
View MediaLibraryPicker(MediaLibraryPickerOptions options);

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
