# Lib-Media roadmap

The goal is to give a video editor the primitives it needs to open media, locate packets, decode and present frames,
encode output, and finish a playable file. The [capability table](../README.md#capabilities) and the individual guides
record current support. Each planned capability has its own format, direction, platform, and I/O requirements.

## Delivery order

| Order | Work | Acceptance |
| --- | --- | --- |
| Current | Chained Ogg, Opus/FLAC/MP3 decoding, native audio packet copy, and WAV export | macOS and Android 12 hardware pass preview, repeated seek, packet copy, and WAV readback. Independent checks verify compressed frames, tag and cover retention, and sample counts. See the [six-platform audio matrix](media-codec.md#opus-flac-and-mp3). |
| Delivered | Track presentation intervals, local fMP4 seek, MP4/MOV packet copy and reordered export | macOS passes fixture integration and independent B-picture readback. Android 12 hardware and iOS Simulator pass headless FFmpeg-generated fMP4 seek and HLG HEVC MOV-to-MP4 copy with identical access units and supported metadata. iOS Simulator decodes SDR Main10 MOV to P010. |
| Remaining acceptance | iPhone camera originals and additional desktop and browser runs | On an iPhone, import a camera-original MOV, verify SDR preview and repeated seeks, then save a packet copy and check its display transform, timing and color metadata independently. Validate HDR presentation separately from metadata retention. Expand Windows, Linux and browser checks. |
| Remaining audio acceptance | Expand native audio backend coverage | Check physical iOS, Linux plugins, and additional browsers. Add Windows decoder mappings when the selected transforms meet the packet and PCM timing contract. |

## File and URL playback

- Verify local files, picker-provided references, and HTTPS on all six platforms. Exercise load and source
  replacement, buffering and error recovery, repeated seeking, looping and natural end, view resize, fitting,
  and rotation. Record container, codec, and protocol behavior for each platform player.
- Add playback rate, audio and subtitle track selection, HTTP request headers, or additional input sources as
  applications need them. Evaluate playlists, picture-in-picture, system media controls, background playback,
  DRM, casting, and downloads as separate features.

See the [player guide](../README.md#media-playback) and its [device checks](../README.md#automated-and-device-checks).

## Decoded playback

- Run real audio and video through the current `DecodedPlayback` queues. Measure first presentation, A/V offset,
  long-run drift, and recovery after underrun. Cover pause and resume, video hold and late-frame handling,
  both track-end orders, and audible drain.
- Verify repeated seeks across demuxing, decoder reset, and playback epoch changes. Check that old submissions are
  rejected and that the preview clears or replaces its previously published image when required.
- Measure audio output interruptions and device changes on physical iOS and Linux, including PipeWire's PulseAudio
  service. Expand browser and audio-device coverage for AudioWorklet, startup, pause, and drain.

See the [decoded playback design](codec-design.md#decoded-playback-boundary) and [HuxerUI #102](https://github.com/HuxerUI/HuxerUI/issues/102).

## Codecs and shared frame formats

- Finish platform acceptance for AAC-LC and H.264. Cover Android encoding hardware, iOS device decoding, independent
  iOS encoder builds and device runs, iOS capability queries, Linux hardware and plugin combinations,
  and browser runtime behavior. Compare each capability claim with actual configuration and session behavior.
- Expand AAC configurations, sample rates, input sample formats, and channel layouts as supported by each backend.
  Verify priming and tail padding through encoding, muxing, decoding, and PCM playback.
- Exercise H.264 profile and B-picture controls, keyframe requests, variable frame intervals, and codec configuration
  changes. Preserve PTS, DTS, duration, and the evidence for independently decodable output. Android and Web H.264
  encoding currently omit DTS. Windows may omit DTS when variable timing makes its codec clock unsuitable.
- Extend Apple HEVC decoding and 10-bit frame output to additional platforms, then cover HEVC encoding. Evaluate ProRes,
  AV1, VP8/VP9, and Vorbis by platform and direction. Add capability and configuration coverage with each codec.
- Extend the frame contract as needed for higher bit depths, visible region, and color and HDR information.
  Evaluate platform-frame import and zero-copy paths against measured workloads, with explicit synchronization,
  ownership, and release requirements.

See the [codec guide](media-codec.md), [codec validation](media-codec.md#validation), and [HuxerUI #20](https://github.com/HuxerUI/HuxerUI/issues/20).

## Container coverage

### Existing containers

| Container | Remaining work |
| --- | --- |
| MP4, M4A, MOV, fMP4 | Incremental or embedded fragment indexes for larger and growing inputs, multiple sample descriptions and configuration changes, additional edit lists and fragmented audio trim, AV1 and other needed mappings, and MOV timecode, timed metadata, and additional display transforms. |
| Matroska, MKA, WebM | Additional mappings such as VP8, VP9, AV1, FLAC, and Vorbis for Matroska, and AV1 and Vorbis for WebM. Extend Opus beyond mapping family zero. Cover files without Cues, timestamp precision beyond the current 1 ms output grid, and subtitle, chapter, attachment, and tag preservation. |
| WAVE and RF64 | RF64 additional-size entries, large-file interoperation, and further PCM or channel layouts where needed. |
| ADTS | Additional AAC configurations and transport variants driven by input samples. |

Across containers, verify multi-track offsets, B-picture ordering, variable frame intervals, audio trim,
seek preroll, and the evidence attached to a seek point. Matroska reads AVC and HEVC in stored order with PTS;
the MP4 writer requires explicit DTS. Support a cross-container export only when the required decode timing is available
or established by the chosen codec path. Preserve rotation, crop, aspect ratio, color, HDR, channel layout, and auxiliary
tracks and metadata where the mapping supports them. Report unsupported preservation requests clearly. Use the existing
`ValidateOutput()` for write validation and add read, seek, or metadata queries when a caller needs to inspect those
capabilities before opening.

### New containers and elementary streams

| Format | Planned coverage |
| --- | --- |
| Ogg extensions | Vorbis, FLAC, Theora, and chained output. |
| Native FLAC extensions | Multichannel decode and additional native metadata blocks. |
| MPEG audio extensions | MP2 frame mapping and additional ID3 frames. |
| Wave64, AIFF, AIFC, CAF | PCM and floating-point descriptions, byte order, channel layout, and file lengths. |
| 3GP | ISO BMFF brands and supported track and codec mappings. |
| MPEG-TS, M2TS, MTS | Programs and tracks, PES assembly, clock wrap and discontinuities, sequential I/O, and seek. |
| AVI and OpenDML AVI | Tracks, timing, indexes, and large-file RIFF segments. |
| FLV | Tags, codec configurations, timestamps, and separately verified codec mappings. |
| Annex B AVC and HEVC | Parameter sets, access unit boundaries, length-prefix conversion, and input with absent timestamps. |

Evaluate standalone SRT, WebVTT, and ASS/SSA, ASF/WMA/WMV, MPEG-PS/VOB, MXF, and HEIF/AVIF item support
against actual ingest and export needs. The [container guide](media-container.md) tracks supported combinations
and endpoint requirements.

## System media library

- Verify Android and iOS selection and publication on devices. Cover mixed and multiple selection, large and
  cloud-backed videos, retained references, legacy Android routes, denied access, cancellation, and unmount.
- Evaluate device-library browsing separately when the editor needs a custom asset browser. Scope enumeration,
  pagination, albums, limited library access, and change notifications. Evaluate additional platform integrations
  against their system library capabilities and application requirements.

See the [media library guide](media-library.md#preview-and-validation) and [HuxerUI #125](https://github.com/HuxerUI/HuxerUI/issues/125).

## Integration and acceptance

- Maintain a platform matrix covering build, runtime, and device acceptance. Exercise SDK `File` and
  `FileReference` inputs, sequential and random-access streams, and the platforms and providers that expose them.
- Use external fixtures and verify output with independent tools and platform players. Include large files above
  4 GiB, short reads and writes, truncation, malformed indexes, cancellation, and I/O failures. Measure long-run
  memory use and verify worker shutdown, pending callbacks, and resource release.
- Keep the preview and export example small. Lib-Media provides packets, seek points, timestamps, codecs, frames,
  playback clocks, and container I/O. Lib-AVKit owns FFmpeg fallback and backend selection, precise frame extraction
  and GOP caches, thumbnails and waveforms, resampling and mixing, compositing and color processing, timeline and
  export orchestration, and HLS/DASH manifests and segment scheduling. The editing application owns projects,
  interactions, undo and redo, and progress UI.
