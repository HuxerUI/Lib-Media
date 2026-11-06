# Media decoding

`<huxerui/media_format.h>` and `<huxerui/media_codec.h>` provide the first implementation stage of
[codec issue #20](https://github.com/HuxerUI/HuxerUI/issues/20). Link `HuxerUI::Media` with the installed public SDK.
No `Install()`, View, Runtime, window, or application Task is needed. Existing MediaPlayer and photo-library APIs are unchanged.

This stage decodes raw AAC-LC access units to CPU PCM and length-prefixed AVC access units to CPU YUV.
It does not encode, demux MP4, load URLs/files, play PCM, or publish textures. A source/demuxer owns packet extraction;
[decoded-input playback #102](https://github.com/HuxerUI/HuxerUI/issues/102) remains the next separate consumer.

## Platform status

| Platform | Adapter | Validation in this change |
| --- | --- | --- |
| Android API 23+ | NDK MediaCodec, no Activity/JNI requirement; standard CPU I420/NV12 buffers | AAC/AVC fixtures ran on a Huawei Android 12 device; full preview builds for arm64/x86_64 |
| macOS | AudioConverter for AAC and VideoToolbox with CoreMedia for H.264 | Headless AAC/AVC fixture tests on Apple Silicon and a full preview build |
| iOS 15+ | AudioConverter for AAC and VideoToolbox with CoreMedia for H.264 | Simulator preview build and AAC/AVC fixture tests. Device decoding remains unvalidated. |
| Windows | System synchronous Media Foundation AAC/H.264 transforms, COM/MF owned on the codec worker | Implemented but not compiled or run on this host; treat as experimental until Windows validation |
| Linux | Optional GStreamer appsrc/parser/installed decoder/converter/appsink pipeline | Implemented but not compiled or run on this host; treat as experimental until Linux validation |
| Web | WebCodecs on the owning JS event loop | Full WASM build and real Chrome C++ decoder/bridge fixture tests; simulated late probe/copy/flush retirement tests |

Availability depends on the requested configuration and installed native media components. `Probe(config, completion)`
initializes, negotiates, and releases a decoder asynchronously. Probe success is neither a resource reservation nor a
promise that a subsequent access unit is valid. Decode-time output negotiation can still fail. Config callbacks are not
used by Probe. A native probe uses its own worker and dispatches completion after it exits. Web dispatches
completion on the owning event loop. There is no UI dispatcher.

Windows currently uses the system synchronous transforms, without DXVA or third-party asynchronous MFT enumeration.
Android rejects opaque/vendor CPU layouts it cannot interpret safely. Apple/Android/Web actual fixture tests do not
validate every hardware model or supported profile/level. Hardware selection/classification is not exposed in this stage.

Linux codecs are disabled by default. Enable `-DHUXERUI_MEDIA_GSTREAMER=ON` with GStreamer app/audio/video development
packages 1.24+ and installed AAC/H.264 parser/decoder plugins. Dependencies are explicit; Lib-Media does not install
packages or bundle FFmpeg. GTK remains a dependency of the existing playback target, but the codec adapter never
initializes GTK or creates a display connection. The adapter checks appsrc item and byte capacity before each
nonblocking push so the codec worker can continue draining appsink. GStreamer is not globally deinitialized on close.

Web requires WebCodecs and the browser's codec support, normally in a secure context. All session methods run on the
creating event loop; callers on other workers must marshal commands. Keep the owning worker/WASM module alive through
cleanup and final callback dispatch. There is no blocking proxy to another event loop.

## Configuration and values

- AAC is raw AAC-LC, mono/stereo, 1024 samples per access unit, with a two-byte AudioSpecificConfig. ADTS, HE-AAC,
  explicit SBR extensions, resampling, and channel mixing are unsupported. Choose S16/F32 and packed/planar PCM;
  sample rate and channel layout must match initialization data. Decoder output includes untrimmed priming/padding;
  the source/container layer owns any trim metadata.
- AVC uses avcC and its 1-, 2-, or 4-byte NAL lengths. The initial subset is progressive 8-bit 4:2:0 Baseline/Main/High,
  no frame-number gaps, at most 16 reference pictures. SPS dimensions and in-band SPS updates are validated against
  configured coded bounds, up to 8192×8192; native codec limits may be lower. Initialization data is limited to 64 KiB.
  Start with a key-marked IDR and do so again after Reset. Submit one complete picture access unit in decode order.
- Video output is tightly copied CPU I420/NV12 with coded dimensions, visible rectangle, strides, pixel aspect ratio,
  and mapped color metadata. Unsupported/unreported color facts stay Unknown. Native bottom/row padding is excluded
  where the adapter can copy the configured picture region. No implicit RGB conversion or native surface lease exists.
- `MediaTimestamp` stores signed ticks and a positive timescale. `Rescale` uses nearest rounding, ties away from zero,
  and rejects overflow. `Compare` compares rational values without floating-point loss. Input PTS can be nonmonotonic;
  output PTS is presentation-ordered and restored to the original packet's exact rational value. Native timestamps
  must match an accepted input after rounding to the adapter's matching timescale (100 ns on Windows, microseconds
  elsewhere). DTS and duration are optional; duration cannot be negative.
- Platform conversion can impose smaller time ranges: Web uses safe-integer microseconds; Windows uses signed 100 ns;
  GStreamer uses nanoseconds and offsets the timeline from its first decoding time to represent negative preroll.
  GStreamer packets must not precede that first decoding time. Unsupported ranges fail, never wrap or silently clamp.
- `BufferReference` retains bytes but does not freeze an application's mutable owner. Accepted input is copied before
  TrySubmit returns. Returned output owns immutable library storage, remains valid after reset/close/cleanup, and can be
  read on other threads. Copying or slicing a plane retains its output credit.

## Queue and lifecycle contract

`TryRead()` has three distinct outcomes:

| Result | Meaning |
| --- | --- |
| Failure(CodecError) | Operational failure or closed handle |
| Success(nullopt) | No output is ready now |
| Success(format / frame / EndOfStream) | One queued event; format precedes dependent frames; EOS appears once |

`TrySubmit()` returns Accepted after copying a complete packet, or WouldBlock without consuming it. Queues include
queued and adapter-retained input copies, plus output reservations for in-flight conversion. Copied-input credit is returned
when its last byte owner releases it; after native submission the session keeps only timing and the output reservation.
Oversized packets and impossible configurations fail rather than blocking
forever. Calls copy bounded data and take short locks; they are not hard-real-time audio callbacks.

Both decoders require at least two copied-input slots. Audio requires at least four output slots and enough output bytes
for four 1024-sample frames: Android native decoding can require more than two units before returning its first PCM frame.
Video conservatively requires 34 output slots and maximum-frame byte reservations: up to 16 native delayed frames, 16 presentation-order
slots, and progress headroom. The initial implementation retains a 16-frame presentation window until Finish, even for
low-delay streams. This is a deliberate conservative bound for the decoding milestone, not a low-latency playback claim.
Defaults are 48 input/output slots, 8 MiB compressed input, and 192 MiB retained output; bytes are reserved lazily.

Retained user frames share the same output pool across Reset. They can stall conversion after a reset until released;
Reset cannot replenish their credit. Bounded adapter scratch copies and native codec allocations are additional to the
pool counters: temporary Annex B conversion is at most four times one packet, and CPU normalization uses a bounded
frame copy. These counters are not a promise of a fixed process RSS or physical driver allocation size. No native frame
is deliberately dropped to relieve queue pressure; unexplained missing/extra output fails the session.

| Operation | Immediate effect | Later observation |
| --- | --- | --- |
| Create | Validate and allocate a Starting session | Initialization wakes readiness or records an error |
| Finish | Seal accepted input, enter Draining | Read remaining output, then one EndOfStream; session stays open |
| Reset | Discard barrier, enter Resetting | Native recreation completes, then wakeup; input is WouldBlock until ready |
| Close / destructor | Closed barrier, no new wakeup admission | Optional cleanup_completed reports actual native release/quiescence |

Finish is idempotent while draining/ended. Pending resets coalesce. Close takes precedence over initialization/reset,
can run inside wakeup, and never waits for native disposal or an admitted callback. An admitted wakeup may finish after
Close returns. A fatal error remains readable in Failed while automatically cleaning up; later Close changes it to
Closed without another completion.

Configure optional `wakeup` and `cleanup_completed` callbacks at creation. They are serialized, run outside locks,
and are never inline on API calls. Each native session runs codec work and wakeups on its own worker. Another
session's blocked native call does not occupy that worker. Callbacks may run before the initiating call returns.
Capture independently owned/weak application state. Wakeups can coalesce; drain output and retry input until empty/full.
Callbacks must not throw or block on codec work. A returned CPU frame's final release only signals readiness; it does not
invoke application callbacks on the releasing thread.

Cleanup completion occurs exactly once after Close, destruction, or fatal failure. Success means library/API-level native
resources have been released, admitted wakeups have returned, and the native session's worker has exited. It does not
certify physical driver/browser cache reclamation. Retained CPU output does not delay it. Release errors report Failure
after application callbacks are quiescent; unsafe stuck native work leaves completion pending. A timeout is never
fabricated as success. On Web, retired promise/copy handlers retain only inert JS-side state and cannot call WASM or
restore an application callback.

After observing completion, allow its dispatch to return. Module unload or worker termination additionally needs the host's
fence over every session and executor. Canceling an application's observation does not cancel native cleanup.

## Reading output

The headers contain complete method contracts. This helper illustrates the three read branches without a polling loop
on a UI thread; call it from the application's scheduled consumer when wakeup signals readiness:

```cpp
#include <huxerui/media_codec.h>

using namespace huxerui::media;

CodecResult<bool> DrainAudio(AudioDecoder& decoder,
    const std::function<void(const AudioFrame&)>& consume) {
  for (;;) {
    auto result = decoder.TryRead();
    if (!result.Succeeded()) return CodecResult<bool>::Failure(result.Error());
    if (!result.Value()) return CodecResult<bool>::Success(false);
    auto& output = *result.Value();
    if (const auto* frame = std::get_if<AudioFrame>(&output)) consume(*frame);
    else if (std::holds_alternative<EndOfStream>(output)) return CodecResult<bool>::Success(true);
  }
}
```

The consumer must arrange its own scheduling and lifetime. Do not capture a reference to a local decoder in config
callbacks and assume Create has returned; native callbacks may race that assignment. A format event is useful for
reconfiguring a consumer, and each frame also carries its complete format. No PCM device or texture is created here.

## Validation

With `HUXERUI_MEDIA_BUILD_TESTS=ON`, native CTest adds `media_codec_contract` and, on enabled native codec platforms,
`media_codec_native`. Node adds `media_codec_web_contract`. The native executable is a non-UI installed-SDK consumer:

```sh
build/tests/tests/media_codec_native_tests tests/fixtures/codec
```

The generated fixtures and provenance are in [tests/fixtures/codec](../tests/fixtures/codec/README.md). Tests cover exact
time conversion/overflow, invalid plane ranges, input snapshots, retained output budgets across reset, logical barriers,
close during initialization/from wakeup, pending/failed cleanup, actual AAC sample/frame counts, B-frame presentation
order, format events, EOS, repeated reset, move/destructor retirement, and CPU data retained through cleanup. Regression
cases exercise allocation failures, matching 100 ns PTS, native worker isolation and exit, tight Apple AAC input
credit, legacy Android output metadata, and appsrc backpressure when GStreamer tests are enabled.

For the browser bridge, serve the repository over localhost and open
[tests/codec_web_native.html](../tests/codec_web_native.html). It decodes the same AAC/AVC fixtures through WebCodecs,
checks CPU copies/counts/EOS, and releases the codecs. With Web tests enabled, build `media_codec_web_native` and serve
its generated `tests/codec_wasm.html` beside the JS/WASM/data files. That test invokes the C++ AudioDecoder and
VideoDecoder APIs, decodes both fixtures, resets both sessions, and observes cleanup while retaining CPU output.
The separate Node test injects late support, flush, and copy completions after retirement. Controlled worker shutdown
still needs dedicated validation. iOS-device, Windows, Linux, hardware diversity, stress/RSS, and release-failure injection
in native platform APIs remain validation work; shared fake-backend failure tests do not replace those checks.

A local Apple Silicon Release archive comparison against the pre-codec commit grew from approximately 0.54 MiB to
1.04 MiB (about 0.50 MiB). These figures exclude system frameworks and vary by toolchain. The existing macOS preview,
which does not call a codec API, contains no decoder/session/native codec entry-point symbols. Web includes the small
codec JS bridge as a pre-JS resource even when C++ codec objects are not linked. Address/undefined-behavior sanitizer
runs of the shared contract and macOS native decoder tests passed; this is not a process-RSS or driver-memory benchmark.
