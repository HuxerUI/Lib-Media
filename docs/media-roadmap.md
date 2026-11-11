# Lib-Media roadmap

Lib-Media provides container I/O, codec sessions, decoded playback, platform playback, and system media
selection and publication. See the [capability table](../README.md#capabilities) for current platform coverage.

## Container and playback regression paths

| Input | Acceptance |
| --- | --- |
| MP4 with multiple rate-one edits and sample descriptions | Keep media timestamps and the description table stable. Map the decoded intervals, seek across gaps and configurations, and preserve the edits and packets through MP4/MOV copy. Verify fragmented AAC head and tail trim through WAV sample counts. |
| VP8/VP9 and Opus in WebM and MKV | Seek with Cues or a bounded Cluster scan. Verify late Opus preroll and Web reference packet backpressure. Compare packets, submillisecond timestamps, and trim after WebM/MKV copy. |
| Matroska HEVC and AAC | Verify HEVC preview and repeated seeks. Keep the source Segment tick for negative video PTS and retain AAC sample timing when copying compatible audio to M4A. |
| Chained Ogg Opus | Keep one track with stable description indices. Restart decoding at each link, preserve its absolute audible start, and verify seek and WAV sample counts. |

The Frames example opens, previews, seeks, copies, and saves supported files. The integration tests exercise
reordered encoding, GPU crop and scale, encoded MP4 readback, and WAV export.
[Container guide](media-container.md) · [Preview integration test](../tests/container_preview.cpp)

## Remaining platform acceptance

- Import a camera-original HEVC MOV on an iPhone. Verify SDR preview, repeated seeks, and packet-copy readback
  with its transform, color metadata, and timing. Exercise HDR display with a compatible surface.
- Complete physical iOS, Windows HEVC, Linux audio-device, Android hardware, and additional browser runs.
  Check codec probes, pause and seek, A/V synchronization, drain, GPU frame reuse, and cleanup.
  Verify VP8/VP9 with the installed Windows and Linux decoders and VP9 on physical iOS hardware.
  [Codec matrix](media-codec.md) · [VP8/VP9 matrix](media-codec.md#vp8-and-vp9) · [GPU video validation](gpu-video.md)
- Verify platform players with local files, picker references, and HTTPS. Check selection, retained references,
  and publication on Android and iOS. [Playback checks](../README.md#automated-and-device-checks) ·
  [Media library checks](media-library.md#preview-and-validation)

## Candidate format work

Use application inputs and independent readback to scope each format, direction, and platform.

- Extend MP4/MOV/fMP4 for needed sample entries, index layouts, edits, and metadata. Extend Matroska/WebM
  for needed codecs and subtitle, chapter, and tag preservation.
- Evaluate additional audio mappings and PCM containers, including Vorbis, multichannel FLAC, AIFF, CAF,
  and Wave64. Evaluate AV1 and ProRes codec adapters by platform and direction.
- Evaluate transport streams, AVI, Annex B, and standalone subtitles against actual ingest requirements.

Lib-AVKit owns backend selection, precise extraction, thumbnails, audio processing, compositing, timeline
and export orchestration, and adaptive streaming. The editing application owns project state and interactions.
