# Media codecs

`<huxerui/media_format.h>` and `<huxerui/media_codec.h>` provide the first implementation stage of
[codec issue #20](https://github.com/HuxerUI/HuxerUI/issues/20). Link `HuxerUI::Media` with the installed public SDK.
No `Install()`, View, Runtime, window, or application Task is needed. Existing MediaPlayer and photo-library APIs are unchanged.

This guide covers AAC-LC, Opus, FLAC, and MP3 decoded to CPU PCM, and length-prefixed AVC and HEVC decoded to CPU YUV.
The same header provides `AudioEncoder` for raw AAC-LC and `VideoEncoder` for H.264. Codec sessions do not
demux MP4, load URLs/files, play PCM, or publish textures. A source/demuxer owns packet extraction;
[decoded-input playback #102](https://github.com/HuxerUI/HuxerUI/issues/102) is an independent consumer.

`VideoDecoderConfig::codec` uses `CodecId` from `<huxerui/media_format.h>` and defaults to `CodecId::H264`.
Use `CodecId::H264` or `CodecId::Hevc`. Other identifiers return `InvalidArgument`. When decoding a
supported demuxed track, assign `MediaTrack::codec` directly to the decoder configuration.

`AudioDecoderConfig::codec` uses the same `CodecId` and defaults to `CodecId::Aac`. Use the audio track's
codec, `initialization_data`, sample rate, and channel layout to configure a demuxed stream.

Apple VideoToolbox also decodes single-layer HEVC with complete out-of-band VPS, SPS and PPS. Set
`VideoDecoderConfig::codec` to `CodecId::Hevc` and supply an hvcC record. Use NV12 or I420 for
8-bit input and P010 for 10-bit input. The first packet after opening or resetting must be an IDR.
P010 stores ten-bit samples in the upper bits of little-endian 16-bit words. VideoToolbox color
attachments are retained on decoded frames. The SDR example converts BT.601/BT.709 frames and
applies container rotation. HDR presentation requires a different display path.

## Implementation layout

The public declarations remain in [`media_codec.h`](../include/huxerui/media_codec.h). Decoder and encoder
implementations use separate translation units, so decoder consumers do not acquire an encoder dependency.

| Files | Responsibility |
| --- | --- |
| [`src/codec/media_codec_decoder.cpp`](../src/codec/media_codec_decoder.cpp) | Public `AudioDecoder` and `VideoDecoder` handles and probes |
| [`src/codec/media_codec_encoder.cpp`](../src/codec/media_codec_encoder.cpp) | Public `AudioEncoder` and `VideoEncoder` handles and probes |
| [`src/codec/media_codec_capabilities.cpp`](../src/codec/media_codec_capabilities.cpp), [`capabilities.h/.cpp`](../src/codec/capabilities.h) | Four typed public queries, initial library constraints, and one-shot discovery scheduling |
| [`src/codec/common.h`](../src/codec/common.h), [`common.cpp`](../src/codec/common.cpp) | Shared codec errors, owned byte storage, and AAC/AVC initialization parsing |
| [`src/execution/executor.h`](../src/execution/executor.h), [`executor.cpp`](../src/execution/executor.cpp) | Workers and event-loop scheduling shared by codec and decoded playback sessions |
| [`src/codec/decoder/`](../src/codec/decoder) | Decoder `backend.h`, paired `format.h/.cpp`, `copy.h/.cpp`, and `session.h/.cpp` |
| [`src/codec/encoder/`](../src/codec/encoder) | Audio/video encoder `backend.h`, paired `format.h/.cpp` and `session.h/.cpp` |
| [`platform/`](../platform) | `media_codec_decoder_<platform>.cpp/.mm`, `media_codec_encoder_<platform>.cpp/.mm`, and `media_codec_capabilities_<platform>.cpp/.mm` implement the selected backend contracts |
| [`platform/web/`](../platform/web) | `media_codec_decoder.js`, `media_codec_encoder.js`, and `media_codec_capabilities.js` implement the corresponding browser bridges |

Backend headers declare platform interfaces; the selected platform source defines their factories. Format validation
and frame-copy declarations have matching implementation files. Encoder code depends on common initialization utilities rather
than decoder internals. iOS and macOS keep independent platform implementations.

## Capability queries

`AudioDecoder`, `VideoDecoder`, `AudioEncoder`, and `VideoEncoder` each provide a static asynchronous
`QueryCapabilities(completion)` and a corresponding typed snapshot. The audio decoder also accepts
`QueryCapabilities(codec, completion)`. AAC uses its existing discovery path. Opus, FLAC, and MP3 use one
48 kHz stereo reference configuration to establish positive evidence. A rejected reference leaves codec availability
and untested choices unknown. Use `Probe()` to check the actual configuration.

```cpp
VideoEncoder::QueryCapabilities([](CodecResult<VideoEncoderCapabilities> result) {
  if (!result.Succeeded()) {
    ReportError(result.Error());
    return;
  }
  const auto capabilities = std::move(result).Value();
  PrepareVideoEncoderChoices(capabilities);
});
```

`ReportError` and `PrepareVideoEncoderChoices` are application functions. Native completion runs after the query's
worker, temporary platform objects, and thread-local cleanup have finished. Web completion is asynchronous on the
creating event loop. Neither path needs a window, Runtime, or `Install()`; applications marshal UI updates themselves
and keep callback captures alive. No session callback, wakeup, or output queue is involved.

- `Supported` means the adapter admits an option, a matching component advertises it, or a reference configuration
  establishes positive evidence. Independent fields do not guarantee that their complete combination will work.
- `Unsupported` means the adapter rejects the option or the required platform facility is definitely absent.
  A successfully returned snapshot can report unavailable codecs; discovery errors use a failed `CodecResult`.
- `Unknown` means discovery cannot decide. It does not disable a later concrete `Probe()`.
- `library_limits` contains fixed validation/adapter constraints. `platform_limits` contains only reported platform
  bounds. Empty optionals stay unknown; no library maximum is presented as a device maximum. Intervals are bounds,
  may contain holes, and can depend on other settings; discrete candidates and complete probes remain necessary.
- `hardware_acceleration` reports whether this adapter can use hardware for some configuration when reliably known.
  It does not identify the implementation a future `Create()` will select. No hardware preference API is added.
- Video encoder profiles expose `no_b_frames`, `automatic_b_frames`, and `bounded_b_frames` separately.
  `maximum_b_frames = 0` means no positive numeric bound is usable; an empty value means the ceiling is unknown.
  A supported automatic policy neither requires B pictures nor implies numeric control.

Query to prepare choices, call `Probe(config, completion)` with the selected combination, then `Create(config)`.
Neither discovery nor Probe reserves resources. Stream validity and decode-time CPU output negotiation are still
checked during processing. Snapshots own their candidate vectors and can outlive the query independently.

| Platform | Discovery evidence and limits |
| --- | --- |
| Windows | The same synchronous system MFT CLSIDs used by sessions; advertised media types, ICodecAPI values/defaults and parameter ranges. Missing metadata remains unknown; no third-party or asynchronous MFT enumeration. |
| Linux | Required installed pipeline elements, the decoder selected by the existing factory-rank/filter rule, or the exact avenc_aac/x264enc encoder; static pad caps and property ranges. No GTK initialization or plugin installation. |
| Android | The component chosen by NDK MediaCodec; API 36 metadata is loaded dynamically when available. Older systems retain unknown detailed limits; the library minimum remains API 23 and video encoding still requires API 28. A Baseline session can retry the system software encoder if the first component rejects configuration. No Activity or JNI requirement. |
| iOS | AudioToolbox format lists/encode ranges, VideoToolbox encoder discovery, hardware decoder discovery, and positive encoder profile/control checks using small reference sessions. Software-only decoder availability and unreported limits remain unknown. |
| macOS | An independent platform implementation using AudioToolbox/VideoToolbox discovery with the same conservative evidence rules. |
| Web | Four AAC or three H.264 reference configuration checks on the owning event loop. Failed/rejected reference checks remain unknown; missing required APIs are unsupported. Global numeric limits and hardware use are unknown. |

Apple/Web reference-session or configuration dimensions are not published as platform size limits. Apple cannot
enforce positive numeric B bounds, and Web cannot enforce numeric Main/High B bounds; discovery reflects those
existing adapter restrictions. Platform compilation and runtime validation are recorded separately below.

## Decoder platform status

| Platform | AAC/H.264 adapters | AAC/H.264 validation |
| --- | --- | --- |
| Android API 23+ | NDK MediaCodec, no Activity/JNI requirement; standard CPU I420/NV12 buffers | AAC/AVC fixtures ran on a Huawei Android 12 device; full preview builds for arm64/x86_64 |
| macOS | AudioConverter for AAC and VideoToolbox with CoreMedia for H.264 | Headless AAC/AVC fixture tests on Apple Silicon and a full preview build |
| iOS 15+ | AudioConverter for AAC and VideoToolbox with CoreMedia for H.264 | Simulator preview build and AAC/AVC fixture tests. Device decoding remains unvalidated. |
| Windows | System synchronous Media Foundation AAC/H.264 transforms, COM/MF owned on the codec worker | Windows 11 installed-SDK builds and platform AAC/AVC fixtures, including the longer synchronization fixtures with tight output pools |
| Linux | GStreamer appsrc/parser/installed decoder/converter/appsink pipeline | Headless installed-SDK tests on Ubuntu 24.04 under WSL with GStreamer 1.24.2, including the longer AAC/AVC synchronization fixtures and appsrc backpressure |
| Web | WebCodecs on the owning JS event loop | Full WASM build and real Chrome C++ decoder/bridge fixture tests; simulated late probe/copy/flush retirement tests |

### Opus, FLAC, and MP3

| Platform | Opus | FLAC | MP3 | Validation for this batch |
| --- | --- | --- | --- | --- |
| macOS | AudioConverter | AudioConverter | AudioConverter | Native decode, PCM preview, repeated seeks, and WAV readback on Apple Silicon |
| Android API 23+ | NDK MediaCodec | NDK MediaCodec | NDK MediaCodec | Huawei Android 12 device passed file selection, PCM preview, repeated seeks, and WAV readback. PCM preview uses AAudio on API 26+. |
| iOS 15+ | AudioConverter | AudioConverter | AudioConverter | Native packet decode on iOS Simulator. Physical device playback remains pending. |
| Windows | Unsupported by the current adapter | Unsupported by the current adapter | Unsupported by the current adapter | The existing synchronous Media Foundation adapter continues to decode AAC. |
| Linux | GStreamer `audio/x-opus` | GStreamer `audio/x-flac` | GStreamer `audio/mpeg` Layer III | Requires an installed decoder accepting the packet caps. Runtime acceptance for these mappings is pending. |
| Web | WebCodecs `opus` | WebCodecs `flac` | WebCodecs `mp3` | Chrome ran the WASM preview, repeated seeks, and WAV export and save. Runtime support depends on the browser. |

The container mappings, metadata, and packet-copy paths are independent of the platform decoder. Use
`Probe(config, completion)` to check the selected stream configuration on the running platform.

Availability depends on the requested configuration and installed platform media components. `Probe(config, completion)`
initializes, negotiates, and releases a decoder asynchronously. Probe success is neither a resource reservation nor a
promise that a subsequent access unit is valid. Decode-time output negotiation can still fail. Config callbacks are not
used by Probe. A desktop or mobile probe uses its own worker and dispatches completion after it exits. Web dispatches
completion on the owning event loop. There is no UI dispatcher.

Windows currently uses the system synchronous transforms, without DXVA or third-party asynchronous MFT enumeration.
The AVC transform uses `MF_LOW_LATENCY` to keep startup buffering within the shared output reservation bound.
Its platform output allocation and CPU-copy scratch are bounded by the larger of four configured frames and 64 KiB;
the system AAC transform requests 48 KiB even for mono/stereo AAC-LC. Normalized output still uses the configured
single-frame reservation. AAC output must contain exactly 1024 samples and match the next accepted timestamp within
one 100 ns Media Foundation tick; that Media Foundation truncation is normalized before restoring the original exact packet timestamp.
Android rejects opaque/vendor CPU layouts it cannot interpret safely. Apple/Android/Web actual fixture tests do not
validate every hardware model or supported profile/level. Discovery reports hardware facts only where reliably known;
session implementation identity and hardware selection remain unexposed.

Linux builds require GStreamer app/audio/video development packages 1.24+. Decoding uses installed AAC/H.264
parser/decoder plugins. Dependencies are explicit; Lib-Media does not install
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
- Opus uses mapping family zero, a complete packet and OpusHead, and mono/stereo output at 48 kHz. The container
  supplies pre-skip and end trim for the consumer to apply. Apple configures AudioConverter to return the complete
  packet, including a 2.5 ms first packet when pre-skip is zero. Reopen or reset when seeking into another Ogg link.
  [AudioConverter priming information](https://developer.apple.com/documentation/audiotoolbox/audioconverterprimeinfo)
- FLAC takes complete native frames and the 34-byte STREAMINFO payload. Sample rate and mono/stereo layout must
  match STREAMINFO. MP3 takes complete MPEG Layer III frames with empty initialization data. After a seek, start at
  the demuxer's `decode_start` to rebuild the MP3 bit reservoir. Apply the track's presentation interval to decoded PCM.
- AVC uses avcC and its 1-, 2-, or 4-byte NAL lengths. The initial subset is progressive 8-bit 4:2:0 Baseline/Main/High,
  continuous complete input, at most 16 reference pictures. An SPS may declare frame-number gaps allowed; that flag
  alone does not imply missing pictures or make incomplete input supported. SPS dimensions and in-band SPS updates are validated against
  configured coded bounds, up to 8192×8192; platform codec limits may be lower. Initialization data is limited to 64 KiB.
  Start with a key-marked IDR and do so again after Reset. Submit one complete picture access unit in decode order.
- Video output is tightly copied CPU I420/NV12 with coded dimensions, visible rectangle, strides, pixel aspect ratio,
  and mapped color metadata. Unsupported/unreported color facts stay Unknown. Platform bottom/row padding is excluded
  where the adapter can copy the configured picture region. No implicit RGB conversion or platform surface lease exists.
- `MediaTimestamp` stores signed ticks and a positive timescale. `Rescale` uses nearest rounding, ties away from zero,
  and rejects overflow. `Compare` compares rational values without floating-point loss. Input PTS can be nonmonotonic;
  output PTS is presentation-ordered and restored to the original packet's exact rational value. Platform timestamps
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
when its last byte owner releases it; after platform submission the session keeps only timing and the output reservation.
Oversized packets and impossible configurations fail rather than blocking
forever. Calls copy bounded data and take short locks; they are not hard-real-time audio callbacks.

Both decoders require at least two copied-input slots. Audio requires at least four output slots and enough output bytes
for four maximum-size decoded packets. The sample bound is 1024 for AAC, 5760 for Opus, the STREAMINFO maximum block
size for FLAC, and 1152 for MP3 at MPEG-1 rates or 576 at lower rates. Android MediaCodec can buffer several packets
before returning its first PCM frame.
Video conservatively requires 34 output slots and maximum-frame byte reservations: up to 16 platform delayed frames, 16 presentation-order
slots, and progress headroom. The initial implementation retains a 16-frame presentation window until Finish, even for
low-delay streams. This is a deliberate conservative bound for the decoding milestone, not a low-latency playback claim.
Defaults are 48 input/output slots, 8 MiB compressed input, and 192 MiB retained output; bytes are reserved lazily.

Retained user frames share the same output pool across Reset. They can stall conversion after a reset until released;
Reset cannot replenish their credit. Bounded adapter scratch copies and platform codec allocations are additional to the
pool counters: temporary Annex B conversion is at most four times one packet, and CPU normalization uses a bounded
frame copy. These counters are not a promise of a fixed process RSS or physical driver allocation size. No platform frame
is deliberately dropped to relieve queue pressure; unexplained missing/extra output fails the session.

| Operation | Immediate effect | Later observation |
| --- | --- | --- |
| Create | Validate and allocate a Starting session | Initialization wakes readiness or records an error |
| Finish | Seal accepted input, enter Draining | Read remaining output, then one EndOfStream; session stays open |
| Reset | Discard barrier, enter Resetting | Platform recreation completes, then wakeup; input is WouldBlock until ready |
| Close / destructor | Closed barrier, no new wakeup admission | Optional cleanup_completed reports actual platform release/quiescence |

Finish is idempotent while draining/ended. Pending resets coalesce. Close takes precedence over initialization/reset,
can run inside wakeup, and never waits for platform disposal or an admitted callback. An admitted wakeup may finish after
Close returns. A fatal error remains readable in Failed while automatically cleaning up; later Close changes it to
Closed without another completion.

Configure optional `wakeup` and `cleanup_completed` callbacks at creation. They are serialized, run outside locks,
and are never inline on API calls. Each desktop or mobile session runs codec work and wakeups on its own worker. Another
session's blocked platform call does not occupy that worker. Callbacks may run before the initiating call returns.
Capture independently owned/weak application state. Wakeups can coalesce; drain output and retry input until empty/full.
Callbacks must not throw or block on codec work. A returned CPU frame's final release only signals readiness; it does not
invoke application callbacks on the releasing thread.

Cleanup completion occurs exactly once after Close, destruction, or fatal failure. Success means library/API-level platform
resources have been released, admitted wakeups have returned, and the desktop or mobile session's worker has exited. It does not
certify physical driver/browser cache reclamation. Retained CPU output does not delay it. Release errors report Failure
after application callbacks are quiescent; unsafe stuck platform work leaves completion pending. A timeout is never
fabricated as success. On Web, retired promise/copy handlers retain only inert JS-side state and cannot call WASM or
restore an application callback.

After observing completion, allow its dispatch to return. Module unload or worker termination additionally needs the host's
fence over every session and executor. Canceling an application's observation does not cancel platform cleanup.

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
callbacks and assume Create has returned; platform callbacks may race that assignment. A format event is useful for
reconfiguring a consumer, and each frame also carries its complete format. No PCM device or texture is created here.

## H.264 video encoding

`VideoEncoder` uses the same asynchronous creation, wakeup, reset barrier, retained-storage, and cleanup rules as
the decoders. It accepts CPU I420/NV12, progressive eight-bit 4:2:0 SDR pictures. Dimensions must be positive, even,
and at most 8192×8192, with a full visible rectangle, square pixels, and no rotation. Each input frame must match the
configured format; row/pixel strides may vary. The library copies active samples before returning `Accepted`.
No resizing, RGB conversion, HDR, GPU surface input, HEVC, muxing, or dynamic stream reconfiguration is included.

Choose `H264Profile::Baseline`, `Main`, or `High`; the default is High. Profile and `max_b_frames` are independent:

| B-picture value | Contract |
| --- | --- |
| `0` (default) | Forbid B pictures; unsupported controls fail rather than enabling them |
| Positive, at most 16 | Bound consecutive B pictures without requiring any; Baseline rejects this configuration |
| `std::nullopt` | Let the platform choose within the bounded processing window; Baseline still produces no B pictures |

The expected frame rate is a positive rational at most 240 fps and guides compression/rate control. PTS and supplied
positive duration remain authoritative, including variable frame intervals. PTS must increase strictly both as exact
rational time and after microsecond rounding. Use a new session to change geometry, profile, bitrate, or color metadata.
`key_frame_interval` is a target distance in pictures. `RequestKeyFrame()` coalesces requests for a future IDR; it does
not identify a specific input picture. Packet `key_frame` is derived from actual IDR syntax.

Output is `VideoEncodeFormat`, then complete `EncodedPacket` access units in decode order, then one `EndOfStream`.
An empty stream can end without a format event. Each NAL uses a four-byte big-endian length; SPS/PPS are in `avcC`.
Repeated SPS/PPS and access-unit delimiters are removed after validation. Optional AVC base-layer extension prefixes
are validated and removed; extension-layer pictures are unsupported. `VideoEncodeFormat` reports actual coded bounds
and the visible rectangle. For example, 1080 visible rows may need 1088 coded rows; use the reported coded dimensions
and initialization data for `VideoDecoderConfig`.

With B pictures, packet PTS can move backward. The session matches each output to its accepted picture and restores
the original rational PTS and duration, rather than attaching timing in FIFO order. DTS is optional and only uses
reliable platform timing. Android/Web leave it absent. Windows uses a bounded fixed codec clock to identify reordered
pictures and omits DTS once variable input timing makes that clock unsuitable for the input timeline. Do not derive
DTS from PTS or assume absent DTS means presentation order. A muxer must handle that limitation explicitly.

The macOS preview enables High profile with automatic B pictures. It submits a bounded group of decoded pictures
and retains one encoded packet until the next DTS establishes its decode duration. It keeps the source PTS and
presentation window on an exact common clock. Export reports a timing error if the adapter does not provide the
DTS required for that mode. The Baseline path explicitly disables reordering.

The default video limits are four copied inputs/64 MiB and eight compressed outputs/32 MiB, with 4 MiB maximum per
packet. Input must fit one packed picture. Output must fit at least two full maximum-packet reservations, or
`max_b_frames + 2` for a positive bound. Adapter controls use a progress window of at most 32 pictures and may require
more headroom. Reserve before platform submission, then reduce the byte charge to the actual payload. A one-byte
slice still holds the entire originating packet credit. Reset does not replenish held credit, and oversized output
fails with `ResourceExhausted`. Platform codec allocations and bounded conversion scratch are additional.

### Video encoder platform limits

| Platform | Backend and controls | Validation |
| --- | --- | --- |
| Windows | System synchronous H.264 MFT; Baseline/Main/High, numeric B bounds 0..2 and Auto | Native round trips, real B reordering, requested IDR, variable PTS, crop, padded input and cleanup on Windows 11 |
| Linux | GStreamer `x264enc` + `h264parse`; Baseline/Main/High, numeric B bounds 0..16 and Auto; bitrate must be integral kbit/s | Headless native round trips and shared contracts on Ubuntu 24.04/WSL; no GTK initialization |
| Android | NDK MediaCodec API 28+; Main/High numeric B bounds require API 29+, with codec confirmation for positive bounds; interpretable packed I420/NV12 input only | Baseline software fallback and MP4 export verified on Android 12; broader hardware validation pending |
| iOS | Independent VideoToolbox implementation; zero or Auto B pictures; positive numeric bounds return `Unsupported` | Build/device validation pending |
| macOS | Independent VideoToolbox implementation with the same B-policy limitation | Native H.264 round trips and two-slot output regression on Apple Silicon |
| Web | WebCodecs quality mode; Baseline with zero/Auto, Main/High with Auto only | Chrome C++/WASM Baseline/Main/High round trips and Node bridge contracts |

The library minimum on Android remains API 23; video encoding resolves newer NDK entry points at runtime and
returns `Unsupported` below API 28. Linux requires installed x264/parser plugins and does not install them for the
application. Apple `MaxFrameDelayCount` bounds retained work; it is not treated as a numeric B-picture bound.
WebCodecs has no portable exact B control, so Main/High with the default zero policy returns `Unsupported`.
Web uses quality mode because realtime mode permits dropped pictures. A nonterminal flush makes progress within
the reserved window without discarding accepted input. Probe success does not guarantee later output negotiation.

### Bounded encoding pump

Configure a session from a supported `VideoFormat` and use a pending frame index or one pending source frame:

```cpp
VideoEncoderConfig config;
config.input = format;
config.profile = H264Profile::High;
config.max_b_frames = std::nullopt;
auto created = VideoEncoder::Create(config);
if (!created.Succeeded()) return;
auto encoder = std::move(created).Value();
```

This scheduled pump drains outputs while retrying input. `pictures` and all referenced session state must remain
alive between calls. The consumer handles configuration and stores or forwards packets within its own capacity;
it must not retain every packet against the encoder pool while waiting for EOS.

```cpp
CodecResult<bool> PumpVideo(VideoEncoder& encoder, std::span<const VideoFrame> pictures,
    std::size_t& next, bool& finishing, const std::function<void(VideoEncodeOutput)>& consume) {
  for (;;) {
    auto read = encoder.TryRead();
    if (!read.Succeeded()) return CodecResult<bool>::Failure(read.Error());
    if (read.Value()) {
      const bool ended = std::holds_alternative<EndOfStream>(*read.Value());
      consume(std::move(*read.Value()));
      if (ended) return CodecResult<bool>::Success(true);
      continue;
    }
    if (next < pictures.size()) {
      auto submitted = encoder.TrySubmit(pictures[next]);
      if (!submitted.Succeeded()) return CodecResult<bool>::Failure(submitted.Error());
      if (submitted.Value() == SubmitStatus::WouldBlock) return CodecResult<bool>::Success(false);
      ++next;
    } else {
      if (!finishing) {
        auto finish = encoder.Finish();
        if (!finish.Succeeded()) {
          if (finish.Error().code == CodecErrorCode::InvalidState) return CodecResult<bool>::Success(false);
          return CodecResult<bool>::Failure(finish.Error());
        }
        finishing = true;
      }
      return CodecResult<bool>::Success(false);
    }
  }
}
```

Initialize `next = 0` and `finishing = false`. Call from scheduled work after readiness, output, or capacity wakeups;
do not busy-wait on the UI/event-loop thread. `true` means EOS was consumed and the pump should stop. `Finish()` seals
input; keep draining until EOS. `Reset()` can discard a running/draining stream immediately, and `Close()` starts
asynchronous cleanup. Use owned/weak callback captures and observe `cleanup_completed` when resource release matters.
The Frames preview's [streaming round trip](../examples/preview/src/decoded_clip.cpp) uses bounded queues. It
submits individual pictures in the Baseline path and groups up to four in the reordered path.

## Validation

With `HUXERUI_MEDIA_BUILD_TESTS=ON`, CTest adds `media_codec_contract` and, on enabled desktop and mobile codec platforms,
`media_codec_native`. Node adds `media_codec_web_contract`. The native executable is a non-UI installed-SDK consumer:

```sh
build/tests/tests/media_codec_native_tests tests/fixtures/codec
```

The generated fixtures and provenance are in [tests/fixtures/codec](../tests/fixtures/codec/README.md). Tests cover exact
time conversion/overflow, invalid plane ranges, input snapshots, retained output budgets across reset, logical barriers,
close during initialization/from wakeup, pending/failed cleanup, actual AAC sample/frame counts, B-frame presentation
order, format events, EOS, repeated reset, move/destructor retirement, and CPU data retained through cleanup. Regression
cases exercise allocation failures, matching 100 ns PTS, session worker isolation and exit, tight Apple AAC input
credit, legacy Android output metadata, and appsrc backpressure when GStreamer tests are enabled.

For the browser bridge, serve the repository over localhost and open
[tests/codec_web_native.html](../tests/codec_web_native.html). It decodes the same AAC/AVC fixtures through WebCodecs,
checks CPU copies/counts/EOS, and releases the codecs. With Web tests enabled, build `media_codec_web_native` and serve
its generated `tests/codec_wasm.html` beside the JS/WASM/data files. That test invokes the C++ AudioDecoder and
VideoDecoder APIs, decodes both fixtures, resets both sessions, and observes cleanup while retaining CPU output.
The separate Node test injects late support, flush, and copy completions after retirement. Controlled worker shutdown
still needs dedicated validation. Physical iOS devices, Linux hardware/plugin diversity, stress/RSS, and release-failure injection
in platform APIs remain validation work; shared fake-backend failure tests do not replace those checks.

On macOS, `media_container_preview` verifies PCM across a zero-pre-skip Ogg link and WAV export from WebM without
Cues. For Web, build `media_container_preview_tests` and serve the generated `tests/container_preview_wasm.html`
with its JS/WASM/data files. The test checks that an application callback runs during packet copy and that the copied
MP3 reopens with the same duration, packet count, and cover metadata.

A local Apple Silicon Release archive comparison against the pre-codec commit grew from approximately 0.54 MiB to
1.04 MiB (about 0.50 MiB). These figures exclude system frameworks and vary by toolchain. The existing macOS preview,
which does not call a codec API, contains no decoder/session/platform codec entry-point symbols. Web includes the small
codec JS bridge as a pre-JS resource even when C++ codec objects are not linked. Address/undefined-behavior sanitizer
runs of the shared contract and macOS native decoder tests passed; this is not a process-RSS or driver-memory benchmark.

H.264 encoding adds `media_video_encoder_contract`, `media_video_encoder_native`, and, when Node is available,
`media_video_encoder_web_contract`. The native test generates padded I420/NV12 input and checks Baseline/Main/High,
real B pictures on Windows/Linux, exact PTS/duration through reordering, variable intervals, non-macroblock crop,
requested IDR, short/empty drain, retained packets, and a decoder round trip with luma error tolerance. Shared tests
inject bad output identity, oversized packets, backpressure, retained slices across reset, blocked startup, close from
wakeup, and independent cleanup errors. Run the encoding subset with:

```sh
ctest --test-dir build/tests -R media_video_encoder --output-on-failure
```

For Web, build `media_video_encoder_web_native` and serve its generated `tests/video_encoder_wasm.html` beside
the JS/WASM files over localhost. The C++ test exercises all available requested profiles and decoder round trips.
Node tests separately cover bounded progress flushes, late callbacks/promises, unsupported controls and malformed
output. Android compilation and static Apple review do not replace device runs; those encoder validations remain pending.

Capability discovery adds `media_codec_capabilities`, `media_codec_capabilities_contract` on native targets, and
`media_codec_capabilities_web_contract` when Node is available. The public API test queries all four codecs without
Runtime or Install. Shared tests cover unknown versus unavailable evidence, independent workers, capture cleanup,
post-TLS completion, discovery failures, and a rejected audio reference followed by support at another rate.
Node checks bounded reference queries, unchanged configuration evidence,
and promise retirement. Run the subset with `ctest --test-dir <build> -R media_codec_capabilities --output-on-failure`.
For a browser, build `media_codec_capabilities_tests` and serve the generated `tests/codec_capabilities_wasm.html`.
Windows/Linux and macOS native queries and Android/Web compilation are validated. iOS device
validation remains pending; prior codec-session validation does not validate the new discovery adapter.
