# Lib-Media

Lib-Media is the media library for [HuxerUI](https://github.com/HuxerUI/HuxerUI). It brings media playback, native
image/video selection, saving to the system photo library, and headless AAC-LC/H.264 decoding into one C++ library.
Decoded-input playback and encoding will build on the shared media values in this repository.

Link **`HuxerUI::Media`** and use APIs in **`huxerui::media`**. Playback and photo-library access use the
**`Install`** application hook. Decoders can run independently of that hook, a UI runtime, or a window.

Repository: [HuxerUI/Lib-Media](https://github.com/HuxerUI/Lib-Media).

- [Capabilities](#capabilities)
- [Platform support](#platform-support)
- [Integration](#integration)
- [Media playback](#media-playback)
- [Media selection and saving](#media-selection-and-saving)
- [Media decoding](#media-decoding)
- [Preview, builds, and validation](#preview-builds-and-validation)
- [Repository layout](#repository-layout)
- [Roadmap](#roadmap)

## Capabilities

| Capability | Current status | Public entry points |
| --- | --- | --- |
| Audio/video playback | Available on Android, iOS, macOS, Windows, Linux, and Web | `UseMediaPlayer()`, `MediaSource`, `VideoSurface()` in [`media_player.h`](include/huxerui/media_player.h) |
| Native image/video selection | Available on Android and iOS | `UseMediaPicker()`, `MediaPickerOptions` in [`media_library.h`](include/huxerui/media_library.h) |
| Save images/videos to the system library | Available on Android and iOS | `UseMediaLibrary()`, `SaveImageAsync()`, `SaveVideoAsync()` in [`media_library.h`](include/huxerui/media_library.h) |
| AAC-LC / H.264 decoding | Implemented for the first stage of [#20](https://github.com/HuxerUI/HuxerUI/issues/20); see [decoder validation](#media-decoding) | `AudioDecoder`, `VideoDecoder`, exact timestamps and CPU frames in [`media_codec.h`](include/huxerui/media_codec.h) / [`media_format.h`](include/huxerui/media_format.h) |
| Decoded-media playback | Planned — [#102](https://github.com/HuxerUI/HuxerUI/issues/102) | PCM output and timestamped video-frame playback; no public API yet |
| Audio/video encoding | Planned after the decoder foundation | No public encoder API yet |

Native selection and saving implement [#125](https://github.com/HuxerUI/HuxerUI/issues/125). Selected media is returned as
SDK `FileReference` values, which can be retained for reading, imported to local files, or passed to `MediaSource` for
playback of supported audio/video content. Saving takes a readable local `File` and creates a new system-library asset.

## Platform support

| Platform | Playback backend / video output | Native selection | Saving | Headless decoder |
| --- | --- | --- | --- | --- |
| Android | Media3 ExoPlayer / TextureView | System Photo Picker; legacy `ACTION_PICK` gallery fallback | MediaStore on Android 10+ | NDK MediaCodec |
| iOS | AVFoundation / AVPlayerLayer | PHPicker on iOS 15+ | Photos add-only access on iOS 15+ | AudioConverter / VideoToolbox |
| macOS | AVFoundation / AVPlayerLayer | Unavailable | Unavailable | AudioConverter / VideoToolbox |
| Windows | Media Foundation MediaEngine / D3D11 frame server and HWND swap chain | Unavailable | Unavailable | Media Foundation transforms (experimental) |
| Linux | GtkMediaFile / GdkTexture through HuxerUI ExternalTexture | Unavailable | Unavailable | GStreamer (opt-in, experimental) |
| Web | HTMLMediaElement / native video element | Unavailable | Unavailable | WebCodecs |

Android's library minimum is API 23; playback uses Media3 `1.9.3` resolved by Gradle. Linux builds require GTK 4.14+,
and Linux playback needs a working GTK media backend. Windows playback needs Media Foundation components and
D3D11-capable hardware. Playback container, codec, and streaming-protocol support follows the native backend.
Applications own network permissions, transport-security policy, browser CORS/mixed-content rules, and any background
playback or audio-session configuration.

Selection and saving have independent availability. On unsupported platforms, their capability query returns
`max_selection_count = 0` and `can_save = false`; operations return `Unsupported`. Playback remains available through
its own adapter. See [selection limits and compatibility](#selection-limits-and-compatibility) for Android differences.

## Integration

Lib-Media uses C++20 and the public HuxerUI SDK. Playback and photo-library access use `ApplicationContext`,
`AppOptions::application_hooks`, and window platform factories. Native photo-library access also uses the SDK's
shared `Result` type, callback-to-Task bridge, native file-reference factories, and window presentation coordination
listed in the [SDK integration guide](docs/medialibrary.md#sdk-integration).

Clone the library beside your application:

```sh
git clone https://github.com/HuxerUI/Lib-Media.git
```

After creating your application target, add the library through the HuxerUI CMake helper:

```cmake
huxerui_use_library(your_app
    TARGET HuxerUI::Media
    PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Media"
)
```

For playback and photo-library access, register the installation hook in the application's startup unit.
`App` below is your root View function:

```cpp
#include <huxerui/huxerui.h>
#include <huxerui/media_player.h>
#include <huxerui/media_library.h>

huxerui::View App();

const huxerui::Application application{
    App,
    {.application_hooks = {huxerui::media::Install}},
};
```

The hook registers platform integration. Put the UI components below in codegen-enabled C++ sources with the
corresponding headers included. The [preview application](examples/preview/src/app.cpp) provides a complete integration.
For headless decoding, include `<huxerui/media_codec.h>` and see [Media decoding](#media-decoding).

### Handles and execution

Obtain `UseMediaPlayer()`, `UseMediaPicker()`, and `UseMediaLibrary()` during composition. Each handle belongs to its
owning composition scope and UI thread; retaining a copy does not keep its session active after unmount. Start commands
from UI events or committed Lifecycle work. Run asynchronous operations through a `TaskScope` on the owning UI thread.

Playback commands return `bool` for acceptance and report subsequent native state through snapshots and events.
Selection and saving return Tasks yielding `MediaLibraryResult<T>`, the SDK's `Result<T, MediaLibraryError>`. Check
`Succeeded()` before reading `Value()` or `Error()`; reading the wrong branch throws `std::logic_error`.

## Media playback

`MediaPlayer` plays local files, retained `FileReference` values, and HTTP/HTTPS sources. Audio playback needs no video
view. Mount `VideoSurface(player)` when video output is needed, and compose your own controls.

```cpp
using namespace huxerui;
using namespace huxerui::media;

[[huxerui::composable]]
View PlayerPane() {
  auto player = UseMediaPlayer();
  auto notice = UseState(std::string{});

  player.OnError([notice](const MediaError& error) { notice = error.message; });
  player.OnEnded([notice] { notice = "Playback finished."; });

  return Column {
    VideoSurface(player).With(Frame{.height = 240.0F}),
    Row {
      Button("Load").OnClick([player, notice] {
        if (!player.Load(MediaSource(File("/absolute/path/to/video.mp4")))) notice = "The player is unavailable.";
      }),
      Button("Play").OnClick([player, notice] {
        if (!player.Play()) notice = "Playback cannot start for the current media.";
      }),
      Button("Pause").OnClick([player, notice] {
        if (!player.Pause()) notice = "Playback cannot be paused right now.";
      }),
    }.With(Spacing(8.0F)),
    Text(notice),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch));
}
```

Replace the example path with an accessible file. Use `MediaSource(reference)` for picker-provided content, including
Web files. A network source is `MediaSource(MediaHttpSource{"https://your-server/video.mp4", {}})`.

### State, controls, and events

- `Load()` replaces the source and enters `Loading`; it does not start playback automatically. `Play()` can record
  playback intent while loading. `Pause()` preserves the current source and position; `Clear()` releases the source.
  Loading and clearing preserve volume, mute, and looping preferences.
- `Snapshot()` observes `MediaPlayerState`, including lifecycle status, progress, seekability, buffering/seeking flags,
  metadata, preferences, and fatal errors. Time values use `MediaTime`, a duration in double-precision seconds.
  Missing duration or metadata means unknown; an empty reported buffer list means no buffered ranges are reported.
- `SeekTo()` requires known seekability and clamps to a known duration. One native seek runs at a time; subsequent
  requests collapse to the latest target. Position comes from the backend. Looping requires a finite, seekable source.
- `OnProgress`, `OnEnded`, and `OnError` are composition-bound subscriptions and allow multiple subscribers. Extra
  arguments are Lifecycle dependencies. Capture stable State handles when no dependencies are supplied.
  Progress delivers an initial snapshot, samples about every 250 ms, and coalesces significant updates.
- Fatal errors enter `Failed`; retry by loading a source again. Recoverable errors are delivered through `OnError`.
  `Ended` denotes natural completion; successful loops do not emit it. Platform interruptions may prevent playback
  while preserving intent, and resumption remains subject to platform policy.

Disconnected commands return `false` and are not queued. Invalid source arguments, nonfinite/negative seek positions,
or volume outside `[0, 1]` throw `std::invalid_argument`. Stale callbacks from replaced sources or retired sessions are
ignored. See [`media_player.h`](include/huxerui/media_player.h) for complete command, state, and subscription contracts.

### Video output and current limits

Give `VideoSurface` bounded layout. It provides no controls, autoplay, or fullscreen behavior. A player can have one
mounted surface; mounting a second is rejected. Removing the surface does not pause playback or release the source,
while unmounting the player's owning scope disconnects the session. `VideoSurfaceProperties` supports `ImageFit` and
horizontal/vertical `Start`, `Center`, or `End` alignment; `Stretch` is invalid for content alignment.

The player retains its source while the backend uses it. Android uses native URIs; Web creates object URLs from native
Files; desktop and Apple adapters use `AsFile()` while retaining the reference. Sources without a native-readable
location return `Unsupported`. Raw sequential streams and custom stream providers are not accepted.

Custom HTTP headers currently return `Unsupported` on every adapter. Android includes progressive ExoPlayer playback,
without the optional HLS/DASH modules. Browser playback may require a user gesture and can report `PlaybackNotAllowed`.
The library does not configure process-wide Apple audio sessions or add background services.

Playlists, speed control, track/subtitle selection, DRM, casting, downloads, PiP, system media controls, and raw decoded
frame/PCM input are not current playback features. Use the separate [codec sessions](#media-decoding) for decoded CPU
output; device playback of that output remains on the [roadmap](#roadmap).

## Media selection and saving

`MediaPicker` opens native image/video selection UI. `MediaLibrary` publishes local images and videos to the system
photo library. They can be used independently of each other and of `MediaPlayer`.

### Selecting media

`PickAsync()` accepts `MediaPickerOptions` with `Images`, `Videos`, or `ImagesAndVideos` and a positive `selection_limit`.
Its Task yields `MediaLibraryResult<std::vector<FileReference>>`. User dismissal succeeds with an empty vector; failures
carry `MediaLibraryError` and are never represented as empty successful selections.

```cpp
using namespace huxerui;
using namespace huxerui::media;

[[huxerui::composable]]
View MediaPickerPane() {
  auto picker = UseMediaPicker();
  auto tasks = UseTaskScope();
  auto files = UseState(std::make_shared<std::vector<FileReference>>());
  auto notice = UseState(std::string("No media selected."));

  return Column {
    Button("Choose media").OnClick([picker, tasks, files, notice] {
      tasks.Launch([picker, files, notice]() -> Task<void> {
        auto result = co_await picker.PickAsync({MediaPickerFilter::ImagesAndVideos, 1});
        if (!result.Succeeded()) {
          notice = result.Error().message;
        } else {
          auto selected = std::move(result).Value();
          notice = selected.empty() ? "Selection dismissed." :
              "Selected " + std::to_string(selected.size()) + " item(s).";
          if (!selected.empty()) files = std::make_shared<std::vector<FileReference>>(std::move(selected));
        }
      });
    }),
    Text(notice),
  }.With(Spacing(8.0F));
}
```

The example retains selected references in composition state; dismissal preserves the previous selection. References
remain usable after picker dismissal and session unmount. Android retains SDK URI access; iOS copies provider files
into SDK-owned temporary storage. Keep a reference alive while using its projected path. Metadata may be unknown, and
references do not promise access after process restart. Prefer `OpenReadAsync()` or `ImportToAsync()` for large videos.

### Selection limits and compatibility

Query `CapabilitiesAsync()` on either handle when preparing controls. `MediaLibraryCapabilities::CanPick()` derives
availability from a positive `max_selection_count`; zero means no native selection is available. `can_save` is separate
and can be true even when selection is unavailable. A capability snapshot does not grant permission or reserve UI.

| Selection path | Behavior and limits |
| --- | --- |
| Android system Photo Picker | Preferred on Android 13+, or Android 11/12 with R SDK extension 2+ and an available picker. Images, videos, and mixed selection use the native maximum. |
| Android legacy `ACTION_PICK` | Opens an installed image or video gallery. Reports `max_selection_count = 1` and accepts single selection only. |
| iOS PHPicker | Supports images, videos, and mixed selection with the requested positive count limit. |

On legacy Android, `Images` opens the image gallery and `Videos` opens the video gallery. `ImagesAndVideos` first uses
the system chooser to offer those two routes and requires handlers for both. A requested media kind can be unavailable
even when another kind has a handler. No document-picker fallback is implicit, and no vendor-specific mixed-media or
multi-select extras are assumed.

`selection_limit` defaults to one; zero is invalid. Larger limits enable multiple selection only where supported.
Requests above native capability return `Unsupported` before presentation, and returned types/counts are validated.
The library does not silently reduce limits or truncate selections. Selection requests no broad photo-library or
storage-read permission. Applications can explicitly offer the SDK FilePicker as a separate document-selection action.

### Saving local files

Obtain a handle with `UseMediaLibrary()`. Inside a launched Task, save a readable local `File source` with:

```cpp
auto result = co_await library.SaveImageAsync(source);
if (!result.Succeeded()) {
  ShowError(result.Error().message);
}
```

`ShowError` is an application callback. Use `SaveVideoAsync(source)` for video. Both Tasks yield
`MediaLibraryResult<void>`: success confirms publication and needs no separate scan/notification. Each save creates
another asset and preserves the source. Keep its contents readable and unchanged until completion. To save a selected
reference, first import it to a local temporary file and retain that file until saving completes.

Android 10+ validates the media, streams it into a pending MediaStore entry, and publishes after writing completes;
no storage permission is requested. iOS requires a nonempty `NSPhotoLibraryAddUsageDescription` in the application's
Info.plist and requests Photos add-only authorization. Missing configuration returns `Unavailable`; denial returns
`PermissionDenied`. The preview includes the usage description.

### Cancellation and ownership

Unmount cancels pending native requests and retires the session. A surviving application Task receives `Unavailable`,
while Task cancellation suppresses delivery. A native picker or permission prompt may remain visible until the system
closes it. Native selection shares the SDK's window presentation coordination with FilePicker.

Canceling a save cannot undo committed publication. Android attempts to delete an unfinished pending entry, subject to
provider cleanup failures. Once an iOS Photos transaction is submitted, it may finish after cancellation; its staged
source remains retained until completion. Partial selection results are released if materializing any item fails.
See the [media library guide](docs/medialibrary.md) and [`media_library.h`](include/huxerui/media_library.h) for details.

## Media decoding

`AudioDecoder` and `VideoDecoder` are move-only sessions with bounded queues. Supply complete access units from a source
or demuxer: raw AAC-LC with AudioSpecificConfig produces mono/stereo S16 or F32 PCM, and length-prefixed H.264 with
`avcC` produces I420 or NV12 video. Returned frames own immutable CPU storage and remain readable after reset and close.

- `Probe(config, completion)` checks a configuration by opening and releasing a temporary decoder.
  `Create()` starts a session asynchronously.
- `TrySubmit()` copies an accepted packet. On `WouldBlock`, keep the packet and retry when the session can accept input.
- `TryRead()` returns a format, frame, or `EndOfStream` event on success. An empty optional means no output is ready yet.
- `Finish()` drains accepted input and emits one `EndOfStream`. `Reset()` discards queued work and restarts decoding.
- `Close()` and destruction retire the session immediately. The optional `cleanup_completed` callback reports teardown
  after admitted wakeups return. On native platforms, it also follows worker exit.

Each native session and probe has its own worker. Web sessions run on the event loop that created them. On Linux, enable
`-DHUXERUI_MEDIA_GSTREAMER=ON` with GStreamer 1.24+ development packages and installed AAC/H.264 parser and decoder plugins.

AAC/H.264 fixture decoding passed on an Android 12 device, an iOS simulator, Apple Silicon macOS, and Chrome through the
C++/WASM API. Windows timestamp matching has a simulated regression test. Native decoder validation is pending on
Windows, Linux, and physical iOS devices. See the [decoding guide](docs/mediacodec.md) for examples, queue limits,
platform requirements, and validation instructions.

## Preview, builds, and validation

The [preview](examples/preview) exercises the library through three tabs:

| Tab | Coverage |
| --- | --- |
| Video | Local/reference/URL sources, playback controls, progress, seeking, volume, looping, and video fitting |
| Music | Audio playback using the same player API without a video surface |
| Library | Native image/video/mixed selection, capability-aware multi-select, retained references, and publication |

The Library tab disables multi-select when the maximum is below two; otherwise it requests up to three items within
the native limit. “Save first selection” imports and publishes a retained reference. “Save image” and “Save video” use
FilePicker explicitly to obtain a source. Switching tabs unmounts the previous session and cancels its pending work.

### Build and run

From `examples/preview`, build a platform supported by the installed SDK and host toolchain:

```sh
huxerui build android --profile debug
huxerui build ios --profile debug
huxerui build macos --profile debug
huxerui build windows --profile debug
huxerui build linux --profile debug
huxerui build web --profile debug
```

Use `huxerui run <platform> --profile debug` for interactive testing. For a connected Android device:

```sh
huxerui devices android
huxerui run android --device <device-id> --profile debug --java-home /path/to/jdk
```

Platform shells follow `huxerui platform add` conventions. Android uses the JDK required by its Gradle project. Apple
adapters compile into the CMake library; the iOS Swift package declares the frameworks required by the final app link.

### Automated and device checks

From the library root, with a configured native compiler and SDK:

```sh
cmake -G Ninja -S . -B build/tests -DHUXERUI_HOME=/path/to/sdk -DHUXERUI_MEDIA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Tests cover playback state transitions, coalesced seeking/progress, looping, stale callbacks, preferences, surface
ownership, and media-library result validation, cancellation, late replies, and session retirement. When Node and the
SDK Web registry are available, CTest also exercises the Web adapter with simulated media elements. Codec tests use
generated AAC/H.264 fixtures and cover queue backpressure, native worker isolation, cleanup ordering, and late WebCodecs
callbacks. See the [codec validation instructions](docs/mediacodec.md#validation) for standalone and browser runs.

Windows adds native muted audio smoke tests and video geometry tests. To check frame transfer and presentation against
a real source, run `build/tests/tests/media_windows_video.exe /path/to/video.mp4` with a short clip under 20 seconds.
It uses an off-screen window, resizes the surface, and waits for natural completion without modifying the source.

Device acceptance should cover local/reference/HTTPS playback, seek/end/loop, source replacement, fitting, and platform
interruptions; image/video/mixed selection, native count limits, dismissal, and reading retained references; and
image/video publication, denied access, invalid sources, and cancellation. Exercise both legacy Android gallery routes
and the mixed chooser. Automated contract tests do not replace native device or browser verification.

## Repository layout

| Path | Contents |
| --- | --- |
| [`include/huxerui/`](include/huxerui) | Public APIs and their full contracts |
| [`src/`](src) | Shared session state, validation, and platform routing |
| [`platform/`](platform) | Native adapters and platform package integration |
| [`examples/preview/`](examples/preview) | Consumer application for playback and photo-library access |
| [`tests/`](tests) | Shared contracts, codec fixtures, and platform checks |
| [`docs/medialibrary.md`](docs/medialibrary.md) | Photo-library integration and lifecycle details |
| [`docs/mediacodec.md`](docs/mediacodec.md) | Decoder usage, ownership, platform limits, and validation |
| [`docs/codec-design.md`](docs/codec-design.md) | Shared lifecycle design and remaining playback/encoding milestones |

## Roadmap

The [codec and decoded-playback design](docs/codec-design.md) records the shared formats, ownership rules, six-platform
lifecycle contract, and delivery order. The AAC/AVC decoding milestone is now implemented, with remaining platform
validation called out in the [decoding guide](docs/mediacodec.md).

Decoded-media playback ([#102](https://github.com/HuxerUI/HuxerUI/issues/102)) will consume these same AudioFrame and
VideoFrame values for PCM device output and timestamped video presentation, with bounded queues, a playback clock,
and A/V synchronization. Encoding and broader codec discovery remain later stages of
[#20](https://github.com/HuxerUI/HuxerUI/issues/20). No public playback-of-frames or encoder API is exported yet.

Existing file/URL playback continues to use native players. HuxerUI owns general file, task, window, and texture
facilities; media-specific behavior stays in Lib-Media.
