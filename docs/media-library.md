# System photo and video libraries

Include `<huxerui/media_library.h>`, link `HuxerUI::Media`, and register `huxerui::media::Install` in `application_hooks`.
`UseMediaPicker()` and `UseMediaLibrary()` return composition-owned handles bound to their original window.
Obtain them during composition, then launch operations through a `TaskScope` from event callbacks or committed Lifecycle setup.
Unmount cancels pending platform requests and makes retained handles unavailable; a still-live application Task receives an `Unavailable` error.

## SDK integration

The installed SDK must provide `Result<T, Error>` (including `void` success), `AwaitCallback`, Android `JavaWindowPlatformModuleFactory`, `HuxerUIFileReference.fromUri`, `HuxerUIView.getActivityResultLauncher`, iOS `ObjectiveCWindowPlatformModuleFactory`, `HUXFileReference.copyFromFileURL:error:`, and `HUXGetPresentationCoordinator`.
The Android backend uses the SDK's Java window module and activity-result launcher; result routing is not reimplemented in the library or application Activity.
The iOS backend uses the SDK coordinator and releases its presentation slot only after platform dismissal completes.
Both share platform presentation admission with the SDK FilePicker.
The library uses only public installed SDK interfaces.

## Availability

Call `CapabilitiesAsync()` on either handle to obtain `MediaLibraryResult<MediaLibraryCapabilities>`.
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

`PickAsync()` accepts `MediaPickerOptions` with `Images`, `Videos`, or `ImagesAndVideos` and a positive `selection_limit`.
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
auto result = co_await picker.PickAsync({
    .filter = MediaPickerFilter::ImagesAndVideos,
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

The snippet is intended inside a launched Task; `ShowError` and `ConsumeReferences` are application callbacks.
It requires `max_selection_count >= 3`; use a limit of one for the legacy Android gallery.
User dismissal succeeds with an empty vector; a completed selection succeeds with one or more references.
An operational failure is never represented by an empty successful vector.
Task cancellation suppresses result delivery entirely, following SDK TaskScope semantics.
Failures return `MediaLibraryError`, distinguishing unsupported capability, permission denial, invalid source, unavailable session/host, I/O failure, and unknown failure.

Selection requests no broad photo-library or storage-read permission.
Android references retain the SDK's URI capability; iOS copies each provider file into SDK-owned temporary storage before the provider callback returns.
References remain usable after the picker is dismissed or its facade is unmounted.
Keep a reference alive while using its projected path; a path by itself does not retain temporary data or access.
Use `FileReference::OpenReadAsync()` or `ImportToAsync()` for large videos instead of reading the whole file into memory.
Available name, size, and MIME metadata come from the SDK and may be unknown; references are not persistent asset identifiers and do not promise access after process restart.
If any selected item cannot be materialized, the selection operation fails and releases partially collected references.

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

The preview's Library tab demonstrates single/multiple selection, all three media filters, retained selection references, and saving images/videos.
On iOS, **Open from Photos** in the Frames tab selects one video for decoded preview and packet copy.
Frames retains the selection while it uses the local file exposed by `FileReference::AsFile()`. A reference without
a local path is imported into a temporary file.
The picker requests the asset's current representation so Photos can avoid a compatibility transcode where possible.
[Photos picker representation](https://developer.apple.com/documentation/photosui/phpickerconfiguration-swift.struct/assetrepresentationmode)
Its multiple-selection toggle is disabled when the platform maximum is below two, and its multiple-selection count is the smaller of three and the reported maximum.
“Save first selection” imports a retained reference to a local temporary file and publishes it, exercising reference I/O after picker dismissal.
“Save image” and “Save video” explicitly use the document picker to obtain a local source; they are separate from system photo-library selection.
Switching tabs tears down the media-library session and cancels pending work.

`media_library_contract` checks option/result validation, empty-success dismissal versus failure, void publication results, derived picker support, independent save availability, error categories, one-shot delivery, cancellation, late replies, session retirement, and inline/reentrant startup.
Device acceptance should additionally cover image/video/mixed selection, multiple results, large/cloud-backed videos, reading after picker dismissal, user dismissal, denied Photos add access, invalid/unreadable source files, and canceled publication.
On devices without the system Photo Picker, verify image and video gallery entry points, both routes of the mixed-media chooser, dismissal, and single-selection controls. Legacy requests above one must fail without opening a gallery.
Exercise FilePicker and platform media requests in both launch orders, dismiss/unmount while a picker is active, and verify published assets in the system photo library.
SDK coordination and Task tests remain responsible for their underlying window and coroutine machinery; library tests do not substitute for platform device checks.
