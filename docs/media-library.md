# System photo and video libraries

Include `<huxerui/media_library.h>`, link `HuxerUI::Media`, and register `huxerui::media::Install` in `application_hooks`.
`UseMediaLibrary()` returns a composition-owned handle for system selection, browsing, and publication.
Obtain it during composition, then launch operations through a `TaskScope` from event callbacks or committed Lifecycle setup.
Unmount cancels pending platform requests and makes retained copies unavailable; a still-live application Task receives an `Unavailable` error.

## SDK integration

The installed SDK must provide `Result<T, Error>` (including `void` success), `AwaitCallback`, Android `JavaWindowPlatformModuleFactory`, `HuxerUIFileReference.fromUri`, `HuxerUIView.getActivityResultLauncher`, iOS `ObjectiveCWindowPlatformModuleFactory`, `HUXFileReference.copyFromFileURL:error:`, and `HUXGetPresentationCoordinator`.
The Android backend uses the SDK's Java window module and activity-result launcher; result routing is not reimplemented in the library or application Activity.
The iOS backend uses the SDK coordinator and releases its presentation slot only after platform dismissal completes.
Both share platform presentation admission with the SDK FilePicker.
The library uses only public installed SDK interfaces.

## Availability

Call `MediaLibrary::CapabilitiesAsync()` to obtain `MediaLibraryResult<MediaLibraryCapabilities>`.
`MediaLibraryResult<T>` is the SDK `Result<T, MediaLibraryError>`: check `Succeeded()`, then read `Value()` or `Error()`.
Reading the wrong branch throws `std::logic_error`; use `std::move(result).Value()` to extract an owned success value.
`MediaLibraryCapabilities::CanPick()` derives selection support from a positive `max_selection_count`; zero means unavailable.
`can_save` reports publication support independently, so an unavailable picker does not disable saving.
Availability describes platform support, not permission authorization or a guarantee that a future operation will succeed.

| Platform | Platform selection | Saving |
| --- | --- | --- |
| Android | Prefer the system Photo Picker on Android 13+, or Android 11/12 with R SDK extension 2+; its maximum comes from the system. Otherwise use an installed `ACTION_PICK` gallery with `max_selection_count = 1`. | Android 10+ MediaStore publication. |
| iOS | PHPicker, with the library's existing iOS 15 deployment minimum. | Photos add-only access, subject to authorization. |
| macOS, Windows, Linux, Web | Unavailable; operations return `Unsupported`. | Unavailable; operations return `Unsupported`. |

This version does not use the Google Play services/OEM Photo Picker backport protocols or silently fall back to a document picker.
The legacy `ACTION_PICK` path works with installed galleries, including OEM galleries, from the library's Android API 23 minimum.
It reports selection availability when an image or video gallery handler exists; a request for an unavailable media kind returns `Unsupported`.
An unavailable platform picker does not prevent saving on a system that supports MediaStore publication.
Applications may explicitly offer the SDK FilePicker as a separate document-selection action.

## Selecting media

`MediaLibrary::PickAsync()` opens the system picker with `SystemMediaPickerOptions`, which accepts `Images`,
`Videos`, or `ImagesAndVideos` and a positive `selection_limit`.
Its Task yields `MediaLibraryResult<std::vector<FileReference>>`.
The default limit is one; a larger limit enables multiple selection.
Limits above platform capability return `Unsupported`, and returned results are checked against the requested limit without silently truncating the selection.
An invalid filter or zero limit throws `std::invalid_argument` before starting a Task.

On the legacy Android path, `Images` opens `ACTION_PICK` for the MediaStore image collection with `image/*`, and `Videos` uses the video collection with `video/*`.
`ImagesAndVideos` offers both routes through the system chooser before opening the selected gallery; it requires handlers for both media kinds.
This avoids relying on galleries honoring a wildcard MIME filter or private mixed-media extras.
The legacy path accepts only `selection_limit = 1`: [`ACTION_PICK`](https://developer.android.com/reference/android/content/Intent#ACTION_PICK) has no portable multiple-selection count contract.
The library does not assume that `EXTRA_ALLOW_MULTIPLE` or vendor extras can enforce a requested maximum. Requests above one return `Unsupported` before presentation.
Android Photo Picker and iOS PHPicker retain bounded multiple selection. All paths validate returned media types and counts; an invalid response fails without truncating it.

```cpp
auto result = co_await library.PickAsync(SystemMediaPickerOptions{
    .filter = MediaFilter::ImagesAndVideos,
    .selection_limit = 3,
});
if (!result.Succeeded()) {
  ShowError(result.Error().message);
} else {
  auto selected = std::move(result).Value();
  if (!selected.empty()) {
    ConsumeReferences(std::move(selected));
  }
}
```

Run the snippet inside a Task using the handle from `UseMediaLibrary()`. `ShowError` and `ConsumeReferences` are application callbacks.
It requires `max_selection_count >= 3`; use a limit of one for the legacy Android gallery.
User dismissal succeeds with an empty vector; a completed selection succeeds with one or more references.
An operational failure is never represented by an empty successful vector.
Task cancellation suppresses result delivery entirely, following SDK TaskScope semantics.
Failures return `MediaLibraryError`, distinguishing unsupported capability, permission denial, invalid source, unavailable session/host, I/O failure, and unknown failure.

Selection requests no broad photo-library or storage-read permission.
Android references retain the SDK's URI capability; iOS copies each provider file into SDK-owned temporary storage before the provider callback returns.
References remain usable after the picker is dismissed or the library session unmounts.
Keep a reference alive while using its projected path; a path by itself does not retain temporary data or access.
Use `FileReference::OpenReadAsync()` or `ImportToAsync()` for large videos instead of reading the whole file into memory.
Available name, size, and MIME metadata come from the SDK and may be unknown; references are not persistent asset identifiers and do not promise access after process restart.
If any selected item cannot be materialized, the selection operation fails and releases partially collected references.

## Browsing the library

`MediaLibraryPicker()` is a HuxerUI view for a bounded screen or navigation destination. `MediaLibraryPickerOptions` takes a
`MediaFilter`, a positive selection limit, and callbacks for confirmation and dismissal. The host owns presentation.
Recents opens first. The album list spans the picker width below the top bar, with rounded bottom corners. It shows
Android MediaStore buckets or iOS Photos collections with matching, accessible assets. Tap outside or press Back
to dismiss it and return to the same grid position. Choosing another album keeps the current grid visible until
the new page arrives, then shows the new album
from the top. The same asset keeps its selection when switching albums. Selection order determines the result order.
With a limit of one, selecting a different asset replaces the previous selection.
For multiple selection, the bottom strip keeps the selected items visible across albums. Tap one to review it in
the order that was present when preview opened. Tap a tile to preview it, or its selection control to select it.
In preview, swipe sideways to move between items. A zoomed photo can be panned; zoom back out to resume paging.
Videos start on explicit playback and stop when leaving the item. Adjacent items use thumbnails, and the current
photo can load a higher-resolution image. Entering and leaving preview preserves the underlying grid and follows
the host motion policy. Close, back, preview, selection, and playback expose separate accessibility actions.

```cpp
return MediaLibraryPicker({
    .filter = MediaFilter::ImagesAndVideos,
    .selection_limit = 5,
    .title = "Choose attachments",
    .style = {.grid_columns = 4, .grid_spacing = 2.0F},
    .on_confirm = [selection, open](std::vector<FileReference> files) {
      selection = std::make_shared<std::vector<FileReference>>(std::move(files));
      open = false;
    },
    .on_cancel = [open] { open = false; },
});
```

The picker inherits the host HuxerUI theme and locale, with English and Simplified Chinese strings. Set
`options.style` to override its top app bar, confirmation or auxiliary button style, or selection colors. An
explicit `TopAppBarStyle` or `ButtonStyle` supplies the complete style for that role. Selection badge colors default
to `primary` and `on_primary`, and the outline defaults to the resolved badge background. Wrap the picker in a
HuxerUI `Theme` to give it a separate light or dark appearance. `options.title` accepts a literal or localized
`StringVariant` for browsing. Preview continues to show the media type or its position.

The grid defaults to three columns in compact viewports, five elsewhere, and a 3-DIP gap. Set
`style.grid_columns` to a positive fixed count and `style.grid_spacing` to a finite, non-negative gap in DIPs.
The same gap applies between rows and columns and may shrink in narrow layouts. Cells stay square as the
width, columns, or gap changes. Updating the theme, title, or style preserves the session.

```cpp
auto confirm = UseEnvironment<ButtonStyle>();
confirm.corner_radii = CornerRadii(22);
options.style.confirm_button = confirm;
return Theme(dark ? MaterialDarkThemeDefinition() : MaterialThemeDefinition(),
             MediaLibraryPicker(options));
```

Android uses MediaStore and needs the read declarations for the application's selected media types.
Use `READ_EXTERNAL_STORAGE` with a maximum SDK of 32 for older devices, `READ_MEDIA_IMAGES` and
`READ_MEDIA_VIDEO` for the relevant Android 13+ media types, and `READ_MEDIA_VISUAL_USER_SELECTED` for
Android 14+ reselection. iOS requires `NSPhotoLibraryUsageDescription`. Set
`PHPhotoLibraryPreventAutomaticLimitedAccessAlert` when using the component's explicit management action.
The component requests authorization from a user action and presents the accessible subset in limited mode.
On Android 14 and later, the mixed photo and video picker offers separate **Photo access** and **Video access**
actions for granting access or managing the selected items for each media type.
The system picker and add-only save flows retain their respective authorization requirements.

`MediaLibrary::CapabilitiesAsync()` reports browsing support in `can_browse` independently of read permission
and system picker availability. For a custom UI, use SDK read permissions, `ListAlbumsAsync()` for album metadata,
and `ListAssetsAsync()` for asset pages. Set `MediaAssetQuery::album_id` to `MediaAlbum::id` for a specific album,
or leave it unset for Recents. Pass `MediaAlbum::cover_id` to `LoadImageAsync()` for the album thumbnail.
Pages sort by capture or creation date,
with Android's date-added fallback and unknown dates last. A cursor belongs to the current session, query, and
library revision. On `StaleCursor`, restart pagination. Use `FindAssetsAsync()` to recheck selected IDs anywhere
in the library. A successful lookup returns the visible subset in input order.

`LoadImageAsync()` produces an aspect-preserving photo or video poster within the supplied physical pixel bounds.
Pass the texture directly to `Image`. A successful null texture means the visible item has no representation for
that request. Grid requests use local thumbnails. Photo preview can request a larger representation with download
progress when the platform reports it. `OpenAssetAsync()` checks readability and returns the current file
representation. Android references retain their URI identity, subject to ongoing system access. iOS may stage a
temporary file retained by the reference. Confirmation prepares all selected files and rechecks their visibility
before delivering them. Missing items are removed and a failed download leaves the remaining selection available
for retry.

`OnChanged()` installs a composition-owned callback. It may coalesce platform notifications and indicates that
the library should be queried again. The picker rechecks visibility on foreground entry and after notifications,
retains selected IDs that remain accessible, and resets its thumbnail cache when visibility or content can change.
The grid starts with one metadata page, requests further pages near the loaded boundary, and loads images for its
mounted cells and a small region around the viewport. Returning from preview restores the current album and scroll
position. Leaving preview or unmounting cancels its requests and releases playback resources.

## Saving local files

`SaveImageAsync(File)` and `SaveVideoAsync(File)` return Tasks yielding `MediaLibraryResult<void>`.
`Succeeded()` means the system photo library confirmed publication; success carries no payload and requires no separate notify/scan call.
Keep the source readable and unchanged until completion. Each successful call creates another asset and preserves the source file.
The library does not transcode, edit, or move the original file.

```cpp
auto saved = co_await library.SaveImageAsync(source);
if (!saved.Succeeded()) {
  ShowError(saved.Error().message);
}
```

Use this inside a launched Task with an application-accessible `File source`; use `SaveVideoAsync()` for video.

Android validates media metadata, streams into a new MediaStore entry marked `IS_PENDING`, closes the output, and publishes by clearing that flag.
It requests no storage permission on supported Android versions.
Failure or cancellation before publication attempts to delete the pending entry; provider failures may prevent cleanup.
Cancellation racing publication may leave a successfully published asset, even though the waiting Task is canceled.

iOS requires a nonempty `NSPhotoLibraryAddUsageDescription` in the application Info.plist; a missing key returns `Unavailable` before requesting authorization.
The preview includes this description.
Saving requests `PHAccessLevelAddOnly`, copies the source into owned temporary storage, and uses a Photos change transaction with `shouldMoveFile = NO`.
Success is reported only from the Photos change completion.
Once a Photos transaction is submitted, it cannot be canceled by canceling the Task; its staged file stays retained until platform completion.
An authorization prompt may also outlive Task cancellation. Denial returns `PermissionDenied` while the Task remains active.

## Preview and validation

The preview's Library tab demonstrates system selection, an in-app album picker, retained references, and publication.
On iOS, **Open from Photos** in the Frames tab selects one video for decoded preview and packet copy.
Frames retains the selection while it uses the local file exposed by `FileReference::AsFile()`. A reference without
a local path is imported into a temporary file.
The picker requests the asset's current representation so Photos can avoid a compatibility transcode where possible.
[Photos picker representation](https://developer.apple.com/documentation/photosui/phpickerconfiguration-swift.struct/assetrepresentationmode)
The in-app multi-select limit is three. The system picker also honors the platform maximum.
“Save first selection” imports a retained reference to a local temporary file and publishes it, exercising reference I/O after picker dismissal.
“Save image” and “Save video” explicitly use the document picker to obtain a local source; they are separate from system photo-library selection.
Switching tabs tears down the media-library session and cancels pending work.

`media_library_contract` checks browse capability, page decoding, cursor errors and shared observer registration, as well
as system picker and publication result validation, cancellation, late replies, and session retirement.
Device acceptance should additionally cover image/video/mixed selection, multiple results, large/cloud-backed videos, reading after picker dismissal, user dismissal, denied Photos add access, invalid/unreadable source files, and canceled publication.
On devices without the system Photo Picker, verify image and video gallery entry points, both routes of the mixed-media chooser, dismissal, and single-selection controls. Legacy requests above one must fail without opening a gallery.
Exercise FilePicker and platform media requests in both launch orders, dismiss/unmount while a picker is active, and verify published assets in the system photo library.
For the in-app picker, check cross-album selection order, a deleted selected item, authorization changes while
backgrounded, album dismissal, preview paging and zoom, and switching the inherited theme while preview is open.
With a large library, measure time to the first fully populated viewport,
rapid-scroll frame times, and memory on the first and subsequent openings.

The Android TAS-AN00 device run covered album browsing, photo preview, video playback, confirmation, and live
insertion and deletion while selecting across albums. The transition checks covered album dropdown dismissal,
returning from preview, and retention of thumbnails during selection and scrolling. The iPhone 17 Pro simulator
covered photo and video confirmation, paging away from a playing video, and switching between photo panning and
paging. A debug run with 10,000 synthetic photos on an Android emulator with software rendering took
about 1.5 s to populate the sampled first viewport on first open and 1.3 s on reopening. Sampled peak process RSS
was 182 MiB, and the 95th-percentile frame time during rapid scrolling was 65 ms. Large-library performance
on physical devices and limited/cloud-backed cases remain device acceptance items.
SDK coordination and Task tests remain responsible for their underlying window and coroutine machinery; library tests do not substitute for platform device checks.
