# Lib-Media

Lib-Media is the media library for [HuxerUI](https://github.com/HuxerUI/HuxerUI). It brings media playback, platform
image/video selection, saving to the system photo library, headless AAC-LC/H.264 encoding and decoding, container I/O,
and decoded PCM output into one C++ library.

Link **`HuxerUI::Media`** and use APIs in **`huxerui::media`**. Register the **`Install`** application hook
for file/URL playback and photo-library access. Codecs, container I/O, and PCM output can run independently of that hook,
a UI runtime, or a window.

Repository: [HuxerUI/Lib-Media](https://github.com/HuxerUI/Lib-Media).

- [Capabilities](#capabilities)
- [Platform support](#platform-support)
- [Integration](#integration)
- [Media playback](#media-playback)
- [Media selection and saving](#media-selection-and-saving)
- [Media decoding](#media-decoding)
- [Media encoding](#media-encoding)
- [Media containers](#media-containers)
- [PCM playback](#pcm-playback)
- [Preview, builds, and validation](#preview-builds-and-validation)
- [Repository layout](#repository-layout)
- [Roadmap](#roadmap)

## Capabilities

| Capability | Current status | Public entry points |
| --- | --- | --- |
| Audio/video playback | Available on Android, iOS, macOS, Windows, Linux, and Web | `UseMediaPlayer()`, `MediaSource`, `VideoSurface()` in [`media_player.h`](include/huxerui/media_player.h) |
| Platform image/video selection | Available on Android and iOS | `UseMediaPicker()`, `MediaPickerOptions` in [`media_library.h`](include/huxerui/media_library.h) |
| Save images/videos to the system library | Available on Android and iOS | `UseMediaLibrary()`, `SaveImageAsync()`, `SaveVideoAsync()` in [`media_library.h`](include/huxerui/media_library.h) |
| AAC-LC / H.264 decoding | Implemented for the first stage of [#20](https://github.com/HuxerUI/HuxerUI/issues/20); see [decoder validation](#media-decoding) | `AudioDecoder`, `VideoDecoder`, exact timestamps and CPU frames in [`media_codec.h`](include/huxerui/media_codec.h) / [`media_format.h`](include/huxerui/media_format.h) |
| HEVC decoding | Apple VideoToolbox with 8-bit or 10-bit 4:2:0 output for single-layer `hvc1` | `CodecId::Hevc`, `VideoPixelFormat::P010` |
| Decoded audio and video playback | [#102](https://github.com/HuxerUI/HuxerUI/issues/102), Android, iOS, macOS, Windows, Linux, and Web | `DecodedPlayback` in [`media_playback.h`](include/huxerui/media_playback.h) |
| AAC-LC audio encoding | Implemented; Windows/Linux native checks and Android/Web builds validated; macOS native tests passed; iOS validation pending | `AudioEncoder`, `AudioEncodeFormat`, and `AudioEncodeSummary` in [`media_codec.h`](include/huxerui/media_codec.h) |
| H.264 video encoding | Implemented; Windows/Linux native and Chrome C++/WASM round trips validated; Android build validated; macOS native tests passed; iOS validation pending | `VideoEncoder`, `VideoEncoderConfig`, `H264Profile`, and `VideoEncodeFormat` in [`media_codec.h`](include/huxerui/media_codec.h) |
| Codec capability discovery | Typed AAC-LC/H.264 snapshots for the current adapter; unreported platform facts remain unknown | `QueryCapabilities()` on the four codec classes in [`media_codec.h`](include/huxerui/media_codec.h) |
| Container demuxing and muxing | MP4/fMP4, QuickTime MOV, Matroska, WebM, WAVE/RF64, ADTS, and Ogg Opus; native FLAC and MP3 input | `MediaDemuxer` and `MediaMuxer` in [`media_container.h`](include/huxerui/media_container.h) |

Platform selection and saving implement [#125](https://github.com/HuxerUI/HuxerUI/issues/125). Selected media is returned as
SDK `FileReference` values, which can be retained for reading, imported to local files, or passed to `MediaSource` for
playback of supported audio/video content. Saving takes a readable local `File` and creates a new system-library asset.

## Platform support

| Platform | Playback backend / video output | Platform selection | Saving | Headless decoder |
| --- | --- | --- | --- | --- |
| Android | Media3 ExoPlayer / TextureView | System Photo Picker; legacy `ACTION_PICK` gallery fallback | MediaStore on Android 10+ | NDK MediaCodec |
| iOS | AVFoundation / AVPlayerLayer | PHPicker on iOS 15+ | Photos add-only access on iOS 15+ | AudioConverter / VideoToolbox |
| macOS | AVFoundation / AVPlayerLayer | Unavailable | Unavailable | AudioConverter / VideoToolbox |
| Windows | Media Foundation MediaEngine / D3D11 frame server and HWND swap chain | Unavailable | Unavailable | Media Foundation transforms (experimental) |
| Linux | GtkMediaFile / GdkTexture through HuxerUI ExternalTexture | Unavailable | Unavailable | GStreamer with installed plugins |
| Web | HTMLMediaElement / HTML video element | Unavailable | Unavailable | WebCodecs |

Android's library minimum is API 23; playback uses Media3 `1.9.3` resolved by Gradle. Linux builds require GTK 4.14+,
libpulse, and GStreamer 1.24+ app/audio/video development packages. Linux file/URL playback needs a working GTK media
backend. Windows playback needs Media Foundation components and
D3D11-capable hardware. Playback container, codec, and streaming-protocol support follows the platform backend.
Applications own network permissions, transport-security policy, browser CORS/mixed-content rules, and any background
playback or audio-session configuration.

Selection and saving have independent availability. On unsupported platforms, their capability query returns
`max_selection_count = 0` and `can_save = false`; operations return `Unsupported`. Playback remains available through
its own adapter. See [selection limits and compatibility](#selection-limits-and-compatibility) for Android differences.

## Integration

Lib-Media uses C++20 and the public HuxerUI SDK. Playback and photo-library access use `ApplicationContext`,
`AppOptions::application_hooks`, and window platform factories. System photo-library access also uses the SDK's
shared `Result` type, callback-to-Task bridge, platform file-reference factories, and window presentation coordination
listed in the [SDK integration guide](docs/media-library.md#sdk-integration).

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

Playback commands return `bool` for acceptance and report subsequent platform state through snapshots and events.
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
- `SeekTo()` requires known seekability and clamps to a known duration. One platform seek runs at a time; subsequent
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

The player retains its source while the backend uses it. Android uses platform URIs; Web creates object URLs from browser
`File` objects; desktop and Apple adapters use `AsFile()` while retaining the reference. Sources without a platform-readable
location return `Unsupported`. Raw sequential streams and custom stream providers are not accepted.

Custom HTTP headers currently return `Unsupported` on every adapter. Android includes progressive ExoPlayer playback,
without the optional HLS/DASH modules. Browser playback may require a user gesture and can report `PlaybackNotAllowed`.
The library does not configure process-wide Apple audio sessions or add background services.

Playlists, speed control, track/subtitle selection, DRM, casting, downloads, PiP, system media controls, and raw decoded
frame/PCM input are outside the file and URL player. Use [codec sessions](#media-decoding) for decoded CPU output
and [DecodedPlayback](#pcm-playback) to play its PCM frames.

## Media selection and saving

`MediaPicker` opens platform image/video selection UI. `MediaLibrary` publishes local images and videos to the system
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
availability from a positive `max_selection_count`; zero means no platform selection is available. `can_save` is separate
and can be true even when selection is unavailable. A capability snapshot does not grant permission or reserve UI.

| Selection path | Behavior and limits |
| --- | --- |
| Android system Photo Picker | Preferred on Android 13+, or Android 11/12 with R SDK extension 2+ and an available picker. Images, videos, and mixed selection use the platform maximum. |
| Android legacy `ACTION_PICK` | Opens an installed image or video gallery. Reports `max_selection_count = 1` and accepts single selection only. |
| iOS PHPicker | Supports images, videos, and mixed selection with the requested positive count limit. |

On legacy Android, `Images` opens the image gallery and `Videos` opens the video gallery. `ImagesAndVideos` first uses
the system chooser to offer those two routes and requires handlers for both. A requested media kind can be unavailable
even when another kind has a handler. No document-picker fallback is implicit, and no vendor-specific mixed-media or
multi-select extras are assumed.

`selection_limit` defaults to one; zero is invalid. Larger limits enable multiple selection only where supported.
Requests above platform capability return `Unsupported` before presentation, and returned types/counts are validated.
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

Unmount cancels pending platform requests and retires the session. A surviving application Task receives `Unavailable`,
while Task cancellation suppresses delivery. A platform picker or permission prompt may remain visible until the system
closes it. Platform selection shares the SDK's window presentation coordination with FilePicker.

Canceling a save cannot undo committed publication. Android attempts to delete an unfinished pending entry, subject to
provider cleanup failures. Once an iOS Photos transaction is submitted, it may finish after cancellation; its staged
source remains retained until completion. Partial selection results are released if materializing any item fails.
See the [media library guide](docs/media-library.md) and [`media_library.h`](include/huxerui/media_library.h) for details.

## Media decoding

`AudioDecoder` and `VideoDecoder` are move-only sessions with bounded queues. Supply complete access units from a source
or demuxer: raw AAC-LC with AudioSpecificConfig produces mono/stereo S16 or F32 PCM, and length-prefixed H.264 with
`avcC` produces I420 or NV12 video. Returned frames own immutable CPU storage and remain readable after reset and close.

- `QueryCapabilities(completion)` discovers candidates and known limits; see [capability queries](docs/media-codec.md#capability-queries).
  `Probe(config, completion)` checks a complete configuration by opening and releasing a temporary decoder.
  `Create()` starts a session asynchronously.
- `TrySubmit()` copies an accepted packet. On `WouldBlock`, keep the packet and retry when the session can accept input.
- `TryRead()` returns a format, frame, or `EndOfStream` event on success. An empty optional means no output is ready yet.
- `Finish()` drains accepted input and emits one `EndOfStream`. `Reset()` discards queued work and restarts decoding.
- `Close()` and destruction retire the session immediately. The optional `cleanup_completed` callback reports teardown
  after admitted wakeups return. On desktop and mobile platforms, it also follows worker exit.

Each desktop or mobile session and probe has its own worker. Web sessions run on the event loop that created them. Linux uses
GStreamer 1.24+ with installed AAC/H.264 parser and decoder plugins.

AAC/H.264 fixture decoding passed on an Android 12 device, an iOS simulator, Apple Silicon macOS, Windows 11,
Ubuntu 24.04 under WSL, and Chrome through the C++/WASM API. Windows and Linux also check the longer synchronization
fixtures with tight output pools. Physical iOS decoding remains unvalidated. See the [decoding guide](docs/media-codec.md) for examples, queue limits,
platform requirements, and validation instructions.

## Media encoding

`AudioEncoder` accepts contiguous interleaved S16 mono/stereo PCM at 44.1 or 48 kHz and emits raw AAC-LC packets,
AudioSpecificConfig, and a final sample summary. `VideoEncoder` accepts CPU I420/NV12 pictures and emits H.264
access units with four-byte NAL lengths and a separate `avcC` description. Neither encoder writes a container.

Video supports Baseline, Main, and High profiles, independently of its B-picture policy: `max_b_frames = 0` forbids B
pictures, a positive value requests an upper bound, and `std::nullopt` allows platform selection. Defaults are High
with zero B pictures. Unsupported controls return an error; Web Main/High requires `std::nullopt` because WebCodecs
cannot enforce a numeric bound. Use `RequestKeyFrame()` to request a later IDR without promising an exact frame.

Submit strictly increasing video PTS and drain output in decode order. With B pictures, output PTS can move backward;
the encoder restores the corresponding input PTS and duration. DTS is present only when reliable platform timing is
available. Accepted input is copied, and returned packet slices keep output credit across reset and close.
Continue draining while submitting to avoid filling either bounded queue.

Linux H.264 encoding requires the installed `x264enc` and `h264parse` plugins. Android video encoding requires API 28+;
Main/High numeric B-picture bounds require API 29+. Apple supports zero or automatic B pictures and has independent
iOS/macOS implementations. See the [encoding guide](docs/media-codec.md#h264-video-encoding) for the platform matrix,
queue limits, timing contract, and a bounded producer/consumer example.

All four codec classes expose asynchronous `QueryCapabilities(completion)`. The typed value snapshots separate
library limits from platform-reported limits, describe independent H.264 B-picture policies, and use
`CodecSupport::Unknown` for facts the platform cannot establish. Use discovery to prepare choices, `Probe()` to
check their complete combination, and `Create()` to allocate a session. Discovery does not require a Runtime,
window, or `Install()`, and does not reserve resources or select a different backend.

## Media containers

`MediaDemuxer` reads tracks and packets from SDK streams. `MediaMuxer` writes packets to a fresh stream and
finalizes it through `Finish()`. Both sessions are synchronous. Use the [container guide](docs/media-container.md)
for format mappings, seek behavior, output requirements, and packet ownership.

## PCM playback

`DecodedPlayback` accepts `AudioFrame` from a decoder or a PCM generator. Use `DecodedPlayback::Create({.audio = audio})`
for an interleaved S16 mono or stereo session. The device opens asynchronously and starts paused. Capture `Epoch()`
before starting the producer. Submit contiguous timestamps with `TrySubmitAudio(epoch, frame)`. An accepted
submission copies the frame so the producer can release its storage.

- Call `Play()` and retry the same frame after `WouldBlock` when playback returns capacity.
- Read `Snapshot()` for device-observed position, outstanding media duration and underruns. Pause preserves pending
  media. During starvation the position freezes after the committed tail and playback resumes after refill. A frame
  blocked by capacity can trigger an earlier start.
- Call `FinishAudio(epoch)` to seal input. `Ended` follows device confirmation of the final media sample and pause.
  `Reset()` returns the new token. Use `cleanup_completed` to observe device release after logical `Close()`.

Android uses AAudio on API 26+. iOS 15+ and macOS use AVAudioEngine. On iOS, configure and activate the
application's `AVAudioSession` before creating a playback session with audio. Restore it and create a fresh
playback session after an interruption or invalidated output route. On Web, use the owning browser window event
loop and call `Play()` from a user gesture. The browser must support `AudioWorklet`, `getOutputTimestamp()`, the
requested PCM rate, and the bundled worklet's blob URL in a secure context. Windows uses WASAPI shared output and
the default render endpoint; the system audio engine converts the PCM rate and channel layout to its mix format.
Its playhead follows `IAudioClock`, while endpoint padding only controls writes. An exhausted device run accepts no
further PCM and is recreated after its media tail is confirmed, so underrun silence does not advance media time.
Linux uses libpulse with an installed PulseAudio-compatible server. Cork acknowledgements preserve queued PCM across
pause. The playhead uses platform stream timing without wall-clock interpolation, and a starved stream is retired after its drain and
final media position are confirmed. A lost connection or output route fails the session even while paused.
Video-only sessions do not require an audio server.
The [native playback test](tests/playback_native.cpp) is a headless example that plays both generated PCM and the AAC
fixture through this API. With tests enabled, run `ctest --test-dir build/tests -R media_playback --output-on-failure`
on macOS, Windows, or Linux.
The native executable also runs against the fixture on Android hardware and the iOS Simulator. The iOS
test injects interruption and output-route notifications during playback and while paused. Physical iOS route and
interruption validation is pending. See the
[playback contract and device mappings](docs/codec-design.md#decoded-playback-boundary) for timing and lifecycle details.

For Linux decoded playback and its native tests:

```sh
cmake -G Ninja -S . -B build/linux-tests -DHUXERUI_HOME=/path/to/sdk \
  -DHUXERUI_MEDIA_BUILD_TESTS=ON
cmake --build build/linux-tests
ctest --test-dir build/linux-tests --output-on-failure
```

To try the Web playback sample with the installed SDK, build the WASM test target and serve the build directory
over localhost. Open `http://127.0.0.1:8000/tests/playback_wasm.html` in Chrome and click **Play generated PCM**.
The sample also checks synthetic NV12/I420 publication and video scheduling against the PCM clock.

```sh
emcmake cmake -G Ninja -S . -B build/web-tests -DHUXERUI_HOME=/path/to/sdk -DHUXERUI_MEDIA_BUILD_TESTS=ON
cmake --build build/web-tests --target media_playback_web_native
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/web-tests
```

### Decoded video and synchronized playback

Use `DecodedPlayback::Create({.video = true})` for video alone and
`DecodedPlayback::Create({.audio = audio, .video = true})` for synchronized playback.
Capture `Epoch()` before producing frames and use that token with `TrySubmitVideo()`, `TrySubmitAudio()` and each
track's finish call. Restart the producer with the token returned by `Reset()`. Stale frames and EOS are rejected.

Drive `PresentVideo()` from the presentation thread while the session is active. It publishes at most one due frame
per call through a synchronous callback, which can upload the frame to a caller-owned `ExternalTexture`. Keep pumping
through drain until `Snapshot()` reports `Ended`. A session retains up to two video frames, and `WouldBlock` leaves the
submitted frame with its producer. Pause, Ended, Reset, and Close preserve any already published image. Replace the
texture on a source change if the new epoch should start blank. Synchronized audio starts after `Play()` and the
first accepted video frame or video EOS, subject to normal audio buffering.

The Windows and Linux Frames previews publish copied RGBA rows through the SDK's platform `PixelTexture`.
The Linux preview uses the installed AAC/H.264 parser and decoder plugins for its decoded A/V sample.
The Web Frames preview uses WebCodecs in a secure context and publishes through the SDK's `VideoFrameTexture`.

## Preview, builds, and validation

The [preview](examples/preview) exercises the library through four tabs.

| Tab | Coverage |
| --- | --- |
| Video | Local/reference/URL sources, playback controls, progress, seeking, volume, looping, and video fitting |
| Music | Audio playback using the same player API without a video surface |
| Library | Platform image/video/mixed selection, capability-aware multi-select, retained references, and publication |
| Frames | Seek local MP4/MOV or the bundled sample, copy compatible packets or encode H.264, and save the prepared movie |

The Library tab disables multi-select when the maximum is below two; otherwise it requests up to three items within
the platform limit. “Save first selection” imports and publishes a retained reference. “Save image” and “Save video” use
FilePicker explicitly to obtain a source. Switching tabs unmounts the previous session and cancels its pending work.

The Frames tab decodes files through `MediaDemuxer` and `DecodedPlayback`. Local MP4, MOV, and fragmented MP4
can use a seek index to restart the decoders and replace the texture for each playback epoch. Effective track ranges
limit visible pictures and PCM to the intersection of sample coverage and edits. The bundled file has reordered H.264
pictures, uneven frame intervals, and AAC priming. Apple platforms also preview rotated SDR HEVC in 8-bit or P010.

“Copy Movie” rewrites up to two compatible tracks into MP4 or MOV with the original access units, timing,
authored edits, and supported visual metadata. This includes HLG and Dolby Vision configuration on the
HEVC packet-copy path. “Encode MP4” encodes eight-bit H.264 to fast-start MP4. macOS allows B pictures and uses
explicit encoder DTS. Other platforms use Baseline without reordering. The output container preserves the display
pixel aspect ratio. Android, Windows, and Web pass AAC packets through. The other platforms encode PCM and require
a known AAC trim summary.

Both actions reopen the finished file before “Save Movie” can publish it. Changing the source or preparing another
result clears the previous export. The separate save action lets the browser picker start from a fresh click.
Browser input is staged in memory and limited to 128 MiB.

The [macOS integration test](tests/container_preview.cpp) checks seeks, presentation edits, DTS export, and packet
copy with the bundled MP4 and rotated Main10 MOV fixtures. The [texture contract test](tests/frame_texture_contract.cpp)
checks cropped, padded CPU frames and successive publications. Android 12 and iOS Simulator passed headless
cross-fragment seek and HLG MOV packet-copy checks. iOS Simulator also decoded the SDR Main10 fixture to P010.
The [roadmap](docs/media-roadmap.md#delivery-order) tracks iPhone camera-original and HDR display acceptance.

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
generated AAC/H.264 fixtures and cover queue backpressure, session worker isolation, cleanup ordering, and late WebCodecs
callbacks. See the [codec validation instructions](docs/media-codec.md#validation) for standalone and browser runs.

Windows adds WASAPI underrun/clock tests at 48 kHz stereo and 44.1 kHz mono, decoded PCM/video/A/V tests, texture
publication tests, platform muted audio smoke tests, and video geometry tests. To check frame transfer and presentation against
a real source, run `build/tests/tests/media_windows_video.exe /path/to/video.mp4` with a short clip under 20 seconds.
It uses an off-screen window, resizes the surface, and waits for natural completion without modifying the source.

Linux Release tests passed on Ubuntu 24.04 under WSL with GStreamer 1.24.2, libpulse 16.1, and WSLg's PulseAudio server.
They cover platform AAC/AVC decoding, PCM/AAC playback, video/A/V scheduling, texture publication, 48 kHz stereo and
44.1 kHz mono cork/drain boundaries, and missing-server cleanup. The full Linux preview also builds with the installed
SDK. Physical Linux output routes, PipeWire's PulseAudio service, and measured display/audio synchronization remain
device acceptance work.

Device acceptance should cover local/reference/HTTPS playback, seek/end/loop, source replacement, fitting, and platform
interruptions; image/video/mixed selection, platform count limits, dismissal, and reading retained references; and
image/video publication, denied access, invalid sources, and cancellation. Exercise both legacy Android gallery routes
and the mixed chooser. Automated contract tests do not replace platform device or browser verification.

## Repository layout

`src` contains feature directories only. Each feature keeps its public `media_*.cpp` implementations beside its
private state, validation, and backend contracts. Shared worker execution lives outside the codec and playback modules.

| Path | Contents |
| --- | --- |
| [`include/huxerui/`](include/huxerui) | Public APIs and their full contracts |
| [`src/format/`](src/format) | `media_format.h` implementation: timestamps, PCM/YUV descriptions, and frame validation |
| [`src/player/`](src/player) | `media_player.h` implementation, player state, platform backend contract, channel transport, and video geometry |
| [`src/library/`](src/library) | `media_library.h` implementation, request state, platform transport contract, and payload validation |
| [`src/codec/`](src/codec) | `media_codec.h` implementations, capability discovery, shared AAC/AVC initialization utilities, and separate decoder/encoder internals |
| [`src/container/`](src/container) | `media_container.h` implementations, SDK stream I/O, and format-specific readers and writers |
| [`src/decoded_playback/`](src/decoded_playback) | `media_playback.h` implementation, audio session, device contract, and video scheduling |
| [`src/execution/`](src/execution) | Desktop and mobile session workers, post-exit cleanup dispatch, and Web event-loop scheduling |
| [`platform/`](platform) | Platform adapters and platform package integration |
| [`examples/preview/`](examples/preview) | Consumer application for playback and photo-library access |
| [`tests/`](tests) | Shared contracts, codec fixtures, and platform checks |
| [`docs/media-library.md`](docs/media-library.md) | Photo-library integration and lifecycle details |
| [`docs/media-codec.md`](docs/media-codec.md) | Codec usage, ownership, platform limits, and validation |
| [`docs/media-container.md`](docs/media-container.md) | Container mappings, I/O requirements, and packet contracts |
| [`docs/codec-design.md`](docs/codec-design.md) | Shared lifecycle design and remaining playback/encoding milestones |

## Roadmap

The [Lib-Media roadmap](docs/media-roadmap.md) tracks remaining work across playback, codecs, containers, and the
system media library, with the next integration milestones and acceptance checks.

The [codec and decoded-playback design](docs/codec-design.md) records the shared formats, ownership rules, six-platform
lifecycle contract, and delivery order. The AAC/AVC decoding milestone is now implemented, with remaining platform
validation called out in the [decoding guide](docs/media-codec.md).

The next stages of decoded playback ([#102](https://github.com/HuxerUI/HuxerUI/issues/102)) expand device and
synchronization measurements. AAC-LC audio encoding and H.264 video encoding have initial implementations; remaining
encoder platform validation and additional codecs are later stages of [#20](https://github.com/HuxerUI/HuxerUI/issues/20).
Capability discovery now covers the currently implemented AAC-LC and H.264 adapters.

Existing file/URL playback continues to use platform players. HuxerUI owns general file, task, window, and texture
facilities; media-specific behavior stays in Lib-Media.
