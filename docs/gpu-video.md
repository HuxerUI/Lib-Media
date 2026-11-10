# GPU video

macOS, Android, iOS, Windows, and supported Web browsers can decode and encode SDR H.264 and HEVC Main using
platform images. Select `VideoStorage::Platform` in `VideoDecoderConfig::output_storage` and
`VideoEncoderConfig::input_storage`. Use Opaque on Android and Web and NV12 on macOS, iOS, and Windows. `Probe()` checks codec
initialization for the supplied configuration. Verify the actual output with representative packets and frames.

| Platform | Decode and process | Encode and present |
| --- | --- | --- |
| macOS | VideoToolbox returns retained IOSurface-backed `CVPixelBuffer` images. Metal samples the two NV12 planes and writes into a pixel buffer pool. | VideoToolbox takes the scaled pixel buffer directly. The preview retains it through `PixelBufferTexture`. |
| Android | MediaCodec renders into a private `AImageReader` surface for per-frame processing. Each acquired image leases its GPU-sampleable `AHardwareBuffer`. GLES samples it through EGLImage and renders the scaled image into another hardware buffer. | GLES draws to the MediaCodec input Surface and submits each frame's presentation time. The Frames preview can render selected codec output directly into the SDK's `SurfaceStreamTexture`. |
| iOS | The iOS VideoToolbox adapter retains IOSurface-backed NV12 images. Its own Metal scaler writes into a pixel buffer pool. | VideoToolbox takes the resulting NV12 buffer. `PixelBufferTexture` retains it for display. |
| Windows | A D3D11-aware Media Foundation decoder returns DXGI samples. The D3D11 video processor crops and scales NV12 textures and converts SDK BGRA8 snapshots to NV12. | A hardware MFT accepts a DXGI buffer on the same device. Preview converts to BGRA on the GPU and publishes through `D3D11Texture` on the same adapter. |
| Web | WebCodecs returns retained browser `VideoFrame` objects. | The SDK `VideoFrameTexture` publishes the browser frame. WebCodecs encodes a reference to its image with the submitted presentation timing. |

The Android decoder image reader requires API 26. The existing encoder negotiation requires API 28, with the
requested B-picture controls and EGL extensions subject to the installed driver and MediaCodec component.
The paths support eight-bit SDR. The CPU codec APIs cover their existing Main10 configurations.
Windows requires a D3D11 video processor, DXGI output samples, and a hardware encoder that accepts the
DXGI device manager and the chosen configuration. Rotation uses the video processor's rotation support.
[Android MediaCodec Surface behavior](https://developer.android.com/reference/android/media/MediaCodec#using-an-output-surface) ·
[Android NDK image reader](https://developer.android.com/ndk/reference/group/media) ·
[Core Video Metal plane mapping](https://developer.apple.com/documentation/corevideo/cvmetaltexturecachecreatetexturefromimage%28_%3A_%3A_%3A_%3A_%3A_%3A_%3A_%3A_%3A%29?language=objc) ·
[Media Foundation D3D11 output](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation) ·
[Media Foundation hardware transforms](https://learn.microsoft.com/en-us/windows/win32/medfound/hardware-mfts)

## Frame ownership

A `VideoFrame` with `Storage() == VideoStorage::Platform` carries the same format, PTS, and duration as a CPU
frame and has empty planes. Pass it to a compatible encoder and hold a copy through any deferred use. The frame
privately retains the platform image, including the Android image lease.

The preview and export examples share an internal GPU transform. It crops to the visible rectangle, applies the
frame's display rotation, and resizes to even dimensions. It completes the GPU work before returning a frame
with the original PTS and duration and a full, oriented visible area.

Decoder output credits last through the final frame copy, including across Reset and Close. The Android
image reader allows four simultaneous image leases per decoder generation and acquires each output before
releasing the next one to the surface. The decoder validates presentation order. Retaining all four frames
can pause further output until a consumer releases a lease. Reset starts a new surface and the retired
reader survives only as long as its retained images. A returned image remains usable after codec cleanup.
Encoder submission takes a frame reference on Accepted. WouldBlock leaves it with the caller.
On Windows, a decoded frame retains its `IMFSample` and texture slice. The encoder's DXGI input buffer retains
the frame for as long as the transform uses it. A hardware transform can need additional input slots for delay.
An Opaque decoder output slot is budgeted at four bytes per coded pixel. For larger input sizes, set
`limits.output_bytes` to at least 34 times that charge; the limit is a reservation and does not allocate
34 hardware images. Size the encoder input byte limit for at least one coded picture.

## Accessing a single frame as an SDK texture

Include `<huxerui/media_codec.h>` and call `MakeExternalTexture(frame)` on an SDR frame with `VideoStorage::Platform`.
On success it returns a `std::shared_ptr<huxerui::ExternalTexture>` with one frame already published. The texture
remains usable after the input frame is released or the decoder is reset or closed. On macOS, iOS, Android,
and Windows, its dimensions match the coded image, which may include padding. Use `frame.format` to crop and
rotate the image for display. Web preserves the browser frame's visible rectangle and uses its display size.
Keep `frame.pts` and any other timing metadata separately.

Pass the texture to HuxerUI `Image` or include the SDK platform texture header and cast to its concrete type for GPU work:

| Platform | SDK texture | GPU access |
| --- | --- | --- |
| macOS | `huxerui::macos::PixelBufferTexture` | `AcquireFrame()` retains the original `CVPixelBuffer` for Metal plane mapping. |
| iOS | `huxerui::ios::PixelBufferTexture` | `AcquireFrame()` retains the original `CVPixelBuffer` for Metal plane mapping. |
| Android | `huxerui::android::GlTexture` | `AcquireFrame()->ImportCurrent()` imports the RGBA8 snapshot in a compatible GLES context. |
| Windows | `huxerui::windows::D3D11Texture` | `AcquireFrame()->TryCopyTo()` copies the BGRA8 snapshot to a compatible D3D11 texture on the same adapter. |
| Web | `huxerui::web::VideoFrameTexture` | `AcquireFrame()` clones a reference to the browser frame. Close the clone after use. |

Keep the returned texture alive through GPU use. On macOS, iOS, and Web it holds the input image and its decoder
output credit, so bound how many textures you retain. Android and Windows convert and snapshot on the GPU only
when this function is called. The preview continues to reuse a live SDK texture. Decode-to-encode workflows can
submit compatible platform frames directly to the encoder.

On Web, create, use, and release platform codecs, frames, and textures on the browser main thread. The SDK
publishes a clone of the original browser frame without calling `VideoFrame.copyTo()` in Lib-Media. The browser
selects the decoder implementation. Probe the codec configuration and release frames promptly so the browser can
reuse its decoder surfaces. [WebCodecs decoder output](https://www.w3.org/TR/webcodecs/#dom-videodecoder-decode)

## Encoding a published SDK frame

Render an opaque SDR image at the encoder's input size and complete producer writes according to the SDK texture's
publication contract. Publish it, then call `MakeVideoFrame(texture, encoder_config.input, pts, duration)` before
publishing again on the same texture. The format describes physical pixels and color, independently of the texture's
logical size. On Web, use the browser frame's visible dimensions and apply orientation before publication.

| Platform | Input texture | Image used by the encoder |
| --- | --- | --- |
| macOS and iOS | `PixelBufferTexture` | The retained IOSurface-backed NV12 buffer goes to VideoToolbox. |
| Android | `GlTexture` | The SDK retains its completed RGBA snapshot. The encoder imports it on the same EGL display and draws it to the MediaCodec input Surface. |
| Windows 10 or later | `D3D11Texture` | `MakeVideoFrame()` copies the opaque BGRA8 snapshot on the encoder's adapter and waits for its GPU conversion to NV12. Supply even pixel dimensions and NV12 output color metadata. |
| Web | `VideoFrameTexture` | The encoder shares the browser frame's media resource and applies the submitted timeline on the browser main thread. |

On the tested Huawei Android 12 device, publishing the same GLES render target a second time returned `EGL_BAD_ACCESS`
from the SDK. Alternating GLES render targets in the same SDK `GlTexture` passed the texture input tests.

Submit the resulting `VideoFrame` to `VideoEncoder::TrySubmit()`. On `WouldBlock`, keep it and retry it when capacity
returns. Later texture publications leave the captured frame intact. On `Accepted`, the encoder retains the image
through platform use. On Web, the input reservation stays charged until WebCodecs returns the corresponding packet
or the encoder closes the browser session. The frame's shared texture owner also retains any decoder reservation
attached by `MakeExternalTexture()`. Treat captured pixels as immutable and release each frame when it is no longer needed.

## Preview and verification

Select **GPU video** in the existing Frames tab. Preview uses the same playback clock and seek epochs.
On Android, an unrotated SDR movie without a container-level crop uses a `SurfaceStreamTexture` directly
as the MediaCodec output. The scheduler releases only the selected picture to the Surface and returns dropped
codec buffers without rendering. The last visible image remains during seek until the new epoch presents its
first picture. Rotated or cropped movies use the GPU transform to apply the container geometry.
**Encode half-size MP4** applies the visible crop and encodes at half the visible width and height, rounded to
even dimensions on macOS, Android, iOS, and supported Windows setups. Web **GPU video** previews and exports
compatible SDR browser frames at their visible size. Use **CPU video** to apply additional container geometry on Web.
Export reads platform images independently of the preview output and reopens the MP4 before enabling Save Movie.
The macOS test publishes into the SDK's PixelBufferTexture. The AppKit renderer's own conversion is outside the
application pixel audit.

`media_video_gpu_tests` runs both codecs headlessly. It checks output storage, all picture times,
MP4 decode timing, visible dimensions, crop and rotation pixels, reset at keyframes, and a retained image
after codec cleanup. It also acquires the single-frame texture through the concrete SDK type after releasing the
source frame and closing the decoder. On Android it imports the snapshot into a separate EGL context.
The pixel audit counts application calls to `CVPixelBufferLockBaseAddress` on macOS and iOS, and decoder
`AMediaCodec_getOutputBuffer` or `glReadPixels` on Android. The audited GPU paths must report zero. The Windows
test verifies DXGI-backed storage and D3D11 texture publication.
The test uses CPU decoding separately to inspect the finished MP4 pixels. Application-side measurements
exclude private copies and memory accounting inside platform codecs, GPU drivers, and the UI renderer.

The `texture` test mode renders two different watermarks and captures both from the same SDK texture before encoding.
MP4 readback checks both watermarks, dimensions, and picture timing. On Web, build `media_video_encoder_web_native`
and open `tests/video_encoder_wasm.html?storage=platform`. It disables `VideoFrame.copyTo()` during encoding and checks
watermarks and timing after retrying a blocked submission and reopening the MP4. It also checks that delayed output
holds the input reservation and that reset and close release in-flight textures.

```sh
cmake -S . -B build/gpu -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHUXERUI_MEDIA_BUILD_TESTS=ON -DHUXERUI_HOME=/path/to/sdk
cmake --build build/gpu --target media_video_gpu_tests
ctest --test-dir build/gpu -R 'media_video_(gpu|texture)' --output-on-failure
```

Use the same CTest selection on Windows with the installed Windows SDK and a hardware graphics adapter.
The texture cases publish colored BGRA8 watermarks from a separate D3D11 device on the same adapter. MP4
readback checks the acquired frame's position, chroma, and timing. An unsupported codec or video processor
skips the corresponding fixture. The Windows video backend and texture test sources cross-compile for x64 and
ARM64. Hardware validation and performance measurements are pending.

For the iOS Simulator, build the target using the iOS CMake configuration and run the executable with
`xcrun simctl spawn booted`. Supply an absolute fixture path, `-` for temporary output, and `gpu` or `texture`.
The simulator passed both H.264 (87 pictures) and HEVC Main (20 pictures) with zero audited pixel accesses.
HEVC also passed the Frames preview and reordered export checks. Use a physical iPhone to validate
camera-original MOV files and to measure device performance.

For Android, configure an arm64-v8a build with the NDK toolchain and push the same executable plus
`tests/fixtures/container/preview-avc-aac.mp4` and `preview-hevc-main.mp4` to a writable device directory.
Run from that directory with TMPDIR set there. Use a fresh output path or `-` for a temporary file.

```sh
./media_video_gpu_tests preview-avc-aac.mp4 - gpu
./media_video_gpu_tests preview-hevc-main.mp4 - gpu
./media_video_gpu_tests preview-avc-aac.mp4 - texture
./media_video_gpu_tests preview-hevc-main.mp4 - texture
```

## CPU and GPU comparison

Build in Release and run each route in its own process. The harness reads the same H.264 source,
crops eight pixels from each edge, scales to the same output dimensions, encodes H.264 Baseline,
and finishes a video-only MP4. The CPU reference uses nearest-neighbor sampling of NV12 planes.
Metal and GLES use linear sampling. Timings include codec and GPU setup and MP4 finish. The source
is staged before the timed region. Process CPU time sums work across its threads, and peak RSS
measures the process rather than the platform codecs or GPU service.

```sh
ffmpeg -f lavfi -i 'testsrc2=size=1920x1080:rate=30:duration=3' -c:v h264_videotoolbox \
  -profile:v high -pix_fmt nv12 -g 30 -bf 0 -b:v 8M -color_primaries bt709 -colorspace bt709 \
  -color_trc bt709 -movflags +faststart avc-1080.mp4
ffmpeg -f lavfi -i 'testsrc2=size=3840x2160:rate=30:duration=3' -c:v h264_videotoolbox \
  -profile:v high -pix_fmt nv12 -g 30 -bf 0 -b:v 20M -color_primaries bt709 -colorspace bt709 \
  -color_trc bt709 -movflags +faststart avc-2160.mp4
./media_video_gpu_tests avc-1080.mp4 - cpu 960 540 4000000
./media_video_gpu_tests avc-1080.mp4 - gpu 960 540 4000000
./media_video_gpu_tests avc-2160.mp4 - cpu 1920 1080 8000000
./media_video_gpu_tests avc-2160.mp4 - gpu 1920 1080 8000000
```

Each route completed 90 pictures with the same presentation timestamps and visible output dimensions.
These are single Release runs on an Apple M1 Pro, an iOS Simulator on that Mac, and a Huawei TAS-AN00
running Android 12. Simulator measurements reflect the host and do not establish iPhone throughput.

| Device | Input → output | Route | Wall time (s) | Process CPU (s) | Peak RSS (MiB) | CPU pixel accesses |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| macOS | 1080p → 960×540 | CPU | 1.00 | 0.99 | 124.3 | 180 |
| macOS | 1080p → 960×540 | GPU | 0.61 | 0.14 | 38.2 | 0 |
| macOS | 4K → 1080p | CPU | 3.48 | 3.61 | 292.7 | 180 |
| macOS | 4K → 1080p | GPU | 0.83 | 0.16 | 46.3 | 0 |
| Android | 1080p → 960×540 | CPU | 3.58 | 6.05 | 142.6 | 90 |
| Android | 1080p → 960×540 | GPU | 1.15 | 1.58 | 88.0 | 0 |
| Android | 4K → 1080p | CPU | 10.88 | 16.80 | 394.9 | 90 |
| Android | 4K → 1080p | GPU | 2.33 | 2.09 | 94.9 | 0 |
| iOS Simulator | 1080p → 960×540 | CPU | 1.39 | 2.19 | 288.4 | 180 |
| iOS Simulator | 1080p → 960×540 | GPU | 0.72 | 1.30 | 302.8 | 0 |
| iOS Simulator | 4K → 1080p | CPU | 4.66 | 7.13 | 760.7 | 180 |
| iOS Simulator | 4K → 1080p | GPU | 1.68 | 3.42 | 810.0 | 0 |

The comparison measures transcode throughput, including the different codec buffer paths. It excludes
interactive presentation.
