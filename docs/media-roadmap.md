# Lib-Media roadmap

Lib-Media provides container I/O, codec sessions, decoded playback, platform playback, and system media
selection and publication. See the [capability table](../README.md#capabilities) for current platform coverage.

## Container and playback regression paths

| Input | Acceptance |
| --- | --- |
| MP4 with multiple rate-one edits and sample descriptions | Keep media timestamps and the description table stable. Map the decoded intervals, seek across gaps and configurations, and preserve the edits and packets through MP4/MOV copy. Verify fragmented AAC head and tail trim through WAV sample counts. |
| VP8/VP9 and Opus in WebM and MKV | Seek with Cues or a bounded Cluster scan. Verify late Opus preroll and Web reference packet backpressure. Compare packets, submillisecond timestamps, and trim after WebM/MKV copy. |
| AV1 Main in WebM, MKV, MP4, and fMP4 | Preview and seek from marked key packets. Compare original packets after copy. Encode and decode 8- and 10-bit SDR with the optional CPU backend, and check fMP4 timing and seek. |
| Matroska HEVC and AAC | Verify HEVC preview and repeated seeks. Keep the source Segment tick for negative video PTS and retain AAC sample timing when copying compatible audio to M4A. |
| Chained Ogg Opus | Keep one track with stable description indices. Restart decoding at each link, preserve its absolute audible start, and verify seek and WAV sample counts. |
| ProRes MOV with LPCM | On macOS, preview and seek the 422 family on the LPCM clock. Exercise 422 profile encoding and 4444/XQ packed 16-bit alpha round trips. Verify MOV picture timing and LPCM readback. |
| AIFF and CAF LPCM | Read and write packed mono/stereo PCM, preview and seek on the sample clock, and verify sample counts with independent Core Audio readback. |
| Mono/stereo ALAC in CAF and M4A | Copy both ways with original packets, the short final packet, and exact trim. On macOS and iOS, preview and seek both containers and verify their F32 WAV samples. |
| Wave64 LPCM | Reuse the PCM sample clock for preview and seek. Verify GUID chunk lengths and seek to a sample past 4 GiB in a sparse file. |
| Vorbis in Ogg, WebM, and Matroska | Prime the overlap at open and after seeking. Preserve original packets, headers, and valid PCM samples on single-stream Ogg and audio-only WebM/MKA copies in both directions. |
| Multichannel FLAC and LPCM | Decode 5.1 and 7.1 to ordered PCM, preserve speaker positions on FLAC packet copy and WAVE-family export, and verify repeated seeks and LPCM round trips through AIFF, CAF, and MOV. |
| MPEG-TS, Annex B, and AVI | Import single-program AVC/HEVC and ADTS AAC, untimed AVC/HEVC, and classic AVI with supported video and PCM. Compare packets on remux and exercise timed seek. |
| SubRip and WebVTT | Round-trip text and timed cues through Matroska. Verify cue metadata, chapters, tags, and attachments in their respective mappings. |
| Multichannel Opus and Vorbis | Verify mapped headers, original packets, speaker order, and valid PCM samples through Ogg and Matroska. |
| FLAC and MP3 encoding | Encode PCM with the optional software backends. Compare FLAC samples and speaker order after readback and verify MP3 delay, padding, and audible sample counts. |

The Frames example opens, previews, seeks, copies, and saves supported files. The integration tests exercise
reordered encoding, GPU crop and scale, encoded MP4 readback, and WAV export.
[Container guide](media-container.md) · [Preview integration test](../tests/container_preview.cpp)

## Remaining platform acceptance

- Import a camera-original HEVC MOV on an iPhone. Verify SDR preview, repeated seeks, and packet-copy readback
  with its transform, color metadata, and timing. Exercise HDR display with a compatible surface.
- Import an iPhone camera-original ProRes MOV through Frames. Check the sample entry and `Probe()` with the device
  configuration, then verify decoded timing, LPCM synchronization, and repeated seeks for supported SDR input.
- Complete physical iOS, Windows HEVC, Linux audio-device, Android hardware, and additional browser runs.
  Check codec probes, pause and seek, A/V synchronization, drain, GPU frame reuse, and cleanup.
  Verify VP8/VP9 with the installed Windows and Linux decoders and VP9 on physical iOS hardware.
  [Codec matrix](media-codec.md) · [VP8/VP9 matrix](media-codec.md#vp8-and-vp9) · [GPU video validation](gpu-video.md)
- Run the AIFF/CAF LPCM and CAF/M4A ALAC regressions on native Windows and Linux. Verify CAF ALAC preview
  on an iOS device. [Audio container verification](media-container.md#verification)
- Verify platform players with local files, picker references, and HTTPS. Check selection, retained references,
  and publication on Android and iOS. For the in-app picker, verify limited access changes, cloud-backed files,
  and first-screen and scrolling performance with large libraries. [Playback checks](../README.md#automated-and-device-checks) ·
  [Media library checks](media-library.md#preview-and-validation)

## Candidate format work

Use application inputs and independent readback to scope each format, direction, and platform.

- Extend MP4/MOV/fMP4 for needed sample entries, index layouts, edits, and metadata. Extend Matroska/WebM
  for needed codecs and subtitle, chapter, and tag preservation.

Lib-AVKit owns backend selection, precise extraction, thumbnails, audio processing, compositing, timeline
and export orchestration, and adaptive streaming. The editing application owns project state and interactions.
