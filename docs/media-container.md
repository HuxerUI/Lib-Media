# Media containers

Include `<huxerui/media_container.h>` and link `HuxerUI::Media`. The two move-only sessions work
with the SDK's synchronous streams and can run on a caller-owned worker without application startup.
Use an SDK build that provides `RandomAccessStream` and `File::OpenRandomAccess()`.
Serialize calls on each session.

`MediaTrack` holds the track identity, clock, presentation edits, and sample descriptions. Each
`MediaSampleDescription` holds the codec, framing, initialization bytes, and audio or video geometry.
Pass the description selected by a packet to the corresponding decoder for a supported mapping.

## Supported combinations

| Container | Packets | Input | Output |
| --- | --- | --- | --- |
| Regular MP4, including audio-only M4A | Raw mono/stereo AAC-LC or ALAC, length-prefixed AVC or HEVC, and AV1 temporal units | Random access, or sequential when movie metadata precedes samples | Random write; fast-start requires random read/write |
| Fragmented MP4 | AAC, AVC, and HEVC | Sequential from initialization. Sized random-access input supports incremental seek, including growing files. | Sequential or random write |
| QuickTime MOV | AAC, AVC, HEVC, ProRes, packed interleaved PCM, timecode, and boxed timed metadata | Random access, or sequential when movie metadata precedes samples | Random write |
| Matroska, including audio-only MKA | Length-prefixed AVC or HEVC, VP8, VP9, AV1, raw AAC-LC, mapping-family-zero Opus, and opaque subtitle samples | Sequential or random access | Sequential or random write |
| WebM | VP8, VP9, AV1, and mapping-family-zero Opus | Sequential or random access | Sequential or random write |
| RIFF WAVE and RF64 | Interleaved U8, S16LE, S24LE, S32LE, or F32LE PCM | Sequential or random access | Random write |
| AIFF | Mono/stereo packed interleaved S16BE, S24BE, or S32BE | Random access, or sequential when `COMM` precedes `SSND` | Random write |
| CAF | Mono/stereo packed interleaved S16, S24, S32, or F32 in either byte order, or ALAC | Sized random access | Random write |
| MPEG-4 ADTS | One raw mono/stereo AAC-LC access unit per frame | Sequential or random access | Sequential or random write |
| Ogg | Mapping-family-zero Opus, including chained logical streams on input | Sequential or random access | Sequential or random write for one logical stream |
| Native FLAC | Complete FLAC frames with STREAMINFO | Sequential or random access, with native seek table support | Random write for whole-frame packet copy |
| MP3 | MPEG Layer III frames, including variable bitrate | Sequential or random access | Random write for whole-track packet copy |

MP4 and MOV use self-contained samples in decode order within each track. The fragmented writer emits an
initialization movie and bounded fragments with nonnegative DTS. ADTS and sequential fragmented MP4 report
`IndexUnavailable` for time-based index queries.

### M4A

Use `ContainerFormat::Mp4` for single-track AAC-LC or ALAC audio. Regular and fast-start single-audio output
use the `M4A ` major brand and can be saved with a `.m4a` suffix. A fast-start file supports sequential reading
and, from a random-access stream, indexed seek. For AAC-LC encoded from PCM, write the raw access units with
their PTS and duration, then pass the encoder summary's priming and padding to `EndTrack()`. Use
`PresentationRange()` on readback to select the audible interval.

The audio-copy example also copies fragmented AAC to regular M4A. Native M4A copies place the index before
the samples. On Web, the example writes the index after the samples to avoid relocating media data.

ALAC uses `CodecId::Alac`, mono or stereo, and the 24-byte `ALACSpecificConfig` in `initialization_data`.
Write each original packet with its actual duration, including a shorter final packet. The packet-copy path
retains the lossless bitstream and sample clock. On macOS and iOS, the same packets and configuration can be passed
to `AudioDecoder` for PCM preview and WAV export. [ALAC decoding](media-codec.md#prores-422-and-alac)

`MediaDemuxer::AudioMetadata()` returns file-level tags and retained cover bytes. Supply them in
`MediaMuxerOptions::audio_metadata` when copying. Regular and fast-start MP4 with a single AAC-LC or ALAC
track write supported tags once in `udta/meta/ilst`. Empty optional metadata boxes leave the track and packets
readable. [QuickTime metadata item list](https://developer.apple.com/documentation/quicktime-file-format/metadata_item_list_atom)

### AIFF and CAF

Use `ContainerFormat::Aiff` for `FORM/AIFF` files (`.aif` or `.aiff`) and `ContainerFormat::Caf` for `.caf`.
Pass one mono or stereo audio track with one description and an integer sample rate. PCM packets contain complete
interleaved sample frames. The reader uses the sample rate as its timescale and random-access seek selects an
exact sample boundary. The writer finalizes the file size and sample count on `Finish()`.
[AIFF specification](https://blog.zamzar.com/wp-content/uploads/2014/10/aiffspecs.pdf) ·
[Core Audio Format specification](https://developer.apple.com/library/archive/documentation/MusicAudio/Reference/CAFSpec/CAF_spec/CAF_spec.html)

To copy ALAC between CAF and M4A, pass the audio track and its `initialization_data`, edits, and original
packets to a muxer for `ContainerFormat::Caf` or `ContainerFormat::Mp4`. The 24-byte ALAC configuration and
compressed packet payloads are retained. The CAF mapping uses one configuration and one contiguous audible
interval starting at presentation time zero. When writing CAF, the muxer intersects the edit with the actual
packet coverage and rounds each retained boundary up to the sample grid, as in PCM preview. This preserves the
valid samples when the M4A edit extends past a short final packet. CAF carries timing and codec configuration on
this path. The separate tag and cover mapping described above is available for M4A.

CAF reads packet sizes and valid sample counts from `pakt`. Packet durations use the number of samples coded
in each ALAC packet. With a nominal packet size of 4096 samples, a final packet containing 2368 samples leaves
1728 uncoded samples in the table's remainder. The reader applies any additional priming and padding through
`MediaTrack::edits` and reports the effective interval through `PresentationRange()`. Use the packet times and
edits together for copy or decoding. For newly authored ALAC, `EndTrack()` can also supply priming and padding.
[ALAC configuration](https://github.com/macosforge/alac/blob/master/ALACMagicCookieDescription.txt)

#### Verification

| Platform | LPCM preview, seek, and container readback, plus CAF/M4A ALAC copy | CAF ALAC preview and seek |
| --- | --- | --- |
| macOS | Apple Silicon run with independent Core Audio and AVFoundation readback | Passed |
| iOS | iOS Simulator run | Passed on Simulator |
| Android | TAS-AN00 device run | — |
| Web | Chrome C++/WASM run | — |
| Windows | x64 and Arm64 cross-compilation of container and preview sources. Native run pending. | — |
| Linux | x64 and Arm64 cross-compilation of container and preview sources. Native run pending. | — |

Run `ctest --test-dir <build> -R '^media_audio_containers$' --output-on-failure` on desktop. On Android or iOS,
run `media_container_preview_tests --audio-containers <fixture-directory>` with the audio files from
`tests/fixtures/container`. On Web, open `container_preview_wasm.html?audio-containers` and select **Run preview checks**.

### Track presentation intervals

`MediaTrack::edits` describes supported rate-one edits in order from presentation time zero. Each entry
has a duration and an optional media start. An empty entry advances presentation without selecting media.
Media intervals are forward and nonoverlapping. An empty edit list gives an identity mapping.

Packet PTS and DTS stay on the media clock. Use `MapToPresentation(track.edits, begin, end, slices)`
on a decoded media interval. Each slice contains the retained media start and its half-open presentation
interval. Equivalent rational clocks are reduced before finding the exact mapping grid. Reuse the output vector
between calls. The result can contain several slices if a frame or sample block crosses an edit boundary.
A copy passes the edits and the original packet times together.
Regular MP4 and MOV output rebase the media clock and edit offsets together when the first DTS is nonzero.

`PresentationRange(track_id)` reports the half-open bounds of the available presented samples. The bounds
can span gaps. Equal endpoints describe an empty interval. An absent result means coverage is unavailable
or the track ID is unknown. The query preserves the cursor and does not build an index. Fragmented input
can report partial coverage. `Duration()` includes declared trailing empty edits. Native FLAC and MP3
provide a range when their total sample count and available gapless trim are known.

MP4 and MOV expose authored AAC trim through edits. A fragmented writer needs those edits at open.
For regular MP4, the encoder's priming and padding can instead be supplied at `EndTrack()`. The writer
converts them to an edit. If both are supplied, they must agree with the packet timeline.

### Sample descriptions

`MediaTrack::descriptions` holds one entry per known configuration in stable one-based index order.
Both `ContainerPacket::sample_description_index` and `TrackDecodeStart::sample_description_index`
refer to this table. MP4 and MOV accept multiple descriptions of one codec for AVC, HEVC, and AAC.
AAC descriptions share a sample rate and channel count. Pass one track per ID with its full table
when opening the muxer.

HEVC tracks retain `hvcC` and their `hvc1` or `hev1` sample entry. With `hvc1`, parameter sets are
in the configuration and the writer rejects in-band parameter sets. `hev1` can carry parameter sets in
samples, and its seek points report the container sync mark. With random input, a validated `hvc1`
IDR can report `Independent`. Presentation rotation combines right-angle movie and track transforms
and is emitted as a track matrix. Packet bytes and coded dimensions retain their original orientation.

Video descriptions also retain `pasp` as `pixel_aspect_ratio` and `colr`, `mdcv`, `clli`, `amve`, `dvcC`, and `dvvC`
as bounded `video_extensions` payloads. The writer retains supported payloads byte for byte on compatible MP4 or MOV
tracks. `colr` accepts `nclc`, `nclx`, and bounded ICC profiles. Dolby Vision configuration is supported on compatible
HEVC sample entries. The example uses aspect ratio for display and keeps HDR files on the packet-copy path.
[QuickTime pixel aspect](https://developer.apple.com/documentation/quicktime-file-format/pixel_aspect_ratio) ·
[QuickTime color parameters](https://developer.apple.com/documentation/quicktime-file-format/color_parameter_atom)

MOV uses the `qt  ` major brand and self-contained media. ProRes retains the Proxy, LT, 422, HQ,
4444, or 4444 XQ sample entry. PCM supports U8, signed 16/24/32-bit, and 32-bit float, with little-
and big-endian variants where applicable. The reader groups consecutive PCM frames in each chunk.
The writer emits packed interleaved LPCM in a version-two sound description. MOV carries `tmcd` timecode and
`mebx` boxed timed metadata as `TrackKind::Data` with `ContainerSample` framing. The reader retains the description
payload, timed samples, and supported `tmcd` and `cdsc` track references. Include the associated tracks when copying
to MOV. `ValidateOutput()` rejects these mappings for MP4. [QuickTime timecode](https://developer.apple.com/documentation/quicktime-file-format/timecode_sample_description)

On macOS the CPU preview decodes progressive SDR Proxy, LT, 422, and HQ pictures and plays mono or stereo LPCM on
the MOV sample clock. Compatible iOS devices preview standard 422 (`apcn`). [ProRes decoding](media-codec.md#prores-422-and-alac)

To write macOS ProRes 422 encoder output, use `ContainerFormat::Mov`. Set the video description to
`CodecId::ProRes`, `PacketFraming::CodecPacket`, sample entry `apcn`, and the geometry from `VideoEncodeFormat`.
Keep `initialization_data` empty. Write one complete frame per sample with its PTS, matching DTS, duration, and sync
flag. An existing packed LPCM track can be written alongside it using the source PCM bytes and exact sample clock.
The macOS integration test opens the encoded MOV with AVFoundation and verifies picture timing, 4:2:2 chroma, and
unchanged LPCM bytes. [ProRes encoding](media-codec.md#prores-422-and-alac)

For WAVE, an extensible track carries the 22-byte format suffix in `initialization_data`. It preserves
the subtype, valid bits, and speaker mask when passed to a WAVE or RF64 writer. WAVE packets are
frame-aligned blocks. ADTS strips transport headers on read and supplies them on write. It provides a
sample-count timeline starting at zero and emits a decode start if the AAC configuration changes.

The container API covers the track and packet fields declared in the header. Tag and cover support includes
M4A, native FLAC, and MP3. The current mappings cover the documented track types, visual metadata, and rate-one edits.
Mirrored, scaled, or perspective movie and track matrices report `Unsupported`.
A format or codec combination outside the table returns a format or support error.

Ogg Opus preserves OpusHead, pre-skip, and final-packet trim. The reader validates page CRCs and
assembles continued packets. Each link begins at the previous link's audible end and emits a
`TrackDecodeStart`, including when its configuration matches an earlier link. Its leading packets
can precede the event's absolute `first_audible_pts` in media time. Random input builds a bounded link
and page index on the first seek query. The 80 ms Opus preroll stays within the selected link.
The writer emits one packet per page for a single logical stream and accepts up to 1 ms of timestamp
rounding for WebM interoperability. Supported streams use mapping family zero.
[Ogg Opus mapping and seeking](https://www.rfc-editor.org/rfc/rfc7845.html#section-4.6)

Native FLAC validates frame geometry, position, and checksums. Random input uses checked `SEEKTABLE`
entries when present, with a bounded frame index as a fallback. The writer retains the compressed frames,
updates STREAMINFO sample and frame-size statistics, and writes a bounded seek table. Frame numbering and
timing must begin at the stream origin. The STREAMINFO PCM checksum belongs to the supplied complete stream.
[FLAC metadata and frame format](https://www.rfc-editor.org/rfc/rfc9639.html#section-8)

MP3 handles MPEG-1, MPEG-2 and MPEG-2.5 Layer III with a fixed sample rate and layout. The reader understands
Xing/Info and VBRI frame counts and recognized LAME delay and padding. It builds a bounded frame index and
returns an earlier reservoir restart point as `decode_start_pts`. The writer requires a restartable first frame,
copies the original audio bytes, and rebuilds Xing/Info frame and byte counts, its seek table, and supplied
gapless information. The demuxer's audio window describes the encoder trim on the coded timeline.

Native FLAC and MP3 expose common text and retained JPEG or PNG front-cover bytes through
`MediaDemuxer::AudioMetadata()`. Supported text fields are title, artist, album, track number, genre, and date.
FLAC uses Vorbis comments and PICTURE. MP3 reads ID3v2.3 and v2.4 and writes ID3v2.4. Copying a track to its
native format rewrites these fields without modifying the compressed audio. The output mapping supports one
track and requires a seekable destination for final metadata. [ID3 frame definitions](https://id3.org/id3v2.4.0-frames)

## Read and seek

Open a `File` or `FileReference` through the SDK. Choose `OpenRandomAccess()` for positioning or for
regular MP4 with metadata after media data. The session takes ownership of the stream, including on open failure.
The SDK determines which file providers expose synchronous streams.

`Tracks()` provides a borrowed view of the known tracks and their sample descriptions. `Read()` returns
a packet or a `TrackDecodeStart` before the first packet of a selected track, a configuration change,
a new Ogg link, or the first packet after a seek or reselection. Use the event's index to configure or
reset the decoder. If supplied, `first_audible_pts` is an absolute lower bound in media time and stays
fixed when seeking within the same sequence. Apply packet trim before mapping the remaining decoded
interval to presentation. A successful empty read means EOF for a fixed input. Packet bytes stay
alive across reads, seeks, and session closure.

`FindSeekPoint()` preserves the read position. Both seek APIs take presentation time. `SeekPoint::presentation_time`
identifies the anchor and `decode_start_pts` identifies the media PTS of the first required packet on the reference
track. For AAC in MP4 or MOV, the reader resolves the edit first and includes the contiguous preceding access
unit when it shares the configuration. For Opus, the presentation anchor respects the known audible start
while decode start retains codec delay or link pre-skip. Other selected tracks can start at different media
times. For AVC and HEVC, an independent point has verified IDR syntax. VP9 seeking uses the container sync
flag and the decoder checks the first frame header. VP8 and AV1 use the same contract, with AV1 requiring a
sequence header before its first displayed key frame.

Fixed fMP4 can be read from the start one fragment batch at a time. On sized random-access inputs, a batch can
include several fragments that share media data. Their sample ranges are checked against complete `mdat` boxes.
Fixed inputs establish final sample coverage at EOF. Preview and export read packets as needed.

Time-based queries on random-access fMP4 build a retained index through verified fragments. The index has a
one-million-sample limit and uses 64-bit byte offsets. Direct single-track `sidx` entries can locate a fragment
when its byte range, timestamps, and independent anchor validate against the actual samples. Other queries use the
incremental scan. Signed composition offsets can place a later fragment's sync sample earlier on the presentation
clock. A query can need the remaining fragments to establish the nearest point in its requested direction.
Index queries preserve the read cursor.

Pass `ContainerInputMode::Growing` to the random-access `Open()` overload for an append-only fMP4. The provider must
expose its current size and newly appended bytes. It must retain the existing prefix and use explicit top-level box sizes.
A successful empty read means the available complete fragments have been consumed. Retry it after an append. An
incomplete header or media box stays outside both the read stream and the index until it is complete.

Matroska and WebM share the EBML reader. The document type selects the mapping. The reader accepts
known- and unknown-sized Segments and Clusters, `SimpleBlock`, `BlockGroup`, and Xiph, fixed, and EBML
lacing. Laced frames are delivered separately in their stored order. Select mapped tracks before reading
when `Tracks()` contains an unknown codec. A sequential input requires Info and Tracks before media.
AVC and HEVC carry a decoder configuration record in CodecPrivate and use length-prefixed packets. VP8 uses complete codec packets
without CodecPrivate. VP9 retains any CodecPrivate payload through packet copy. Use container sync marks
when submitting VP8 or VP9 for indexed output.

The reader converts stored Block time to media PTS using the track's CodecDelay and leaves DTS absent.
BlockDuration, DefaultDuration, and known codec frame durations supply timing where available. Opus
retains OpusHead, the track's `seek_preroll`, and signed `discard_padding`. When a writer records the
audible duration of a trimmed Opus Block, the reader can recover its coded duration from the packet.

Random Matroska uses Cues, including discoverable tail Cues. Sized random-access Matroska and WebM can scan
Cluster headers and Block prefixes when a track lacks Cues. After resolving a video Cue, the reader looks near
that Cue for the selected Opus track's preroll. Late audio can start after the video anchor. If an optional
audio search reaches its scan limit, the video Cue still determines the seek. `first_audible_pts` can be absent
until the audio track's start is known. Without Cues for the reference track, the fallback scans from the first Cluster and retains
per-track anchors for later seeks. WebM and audio tracks extend that index as needed on ordered timelines. Matroska video
without Cues completes the bounded scan on the first index query so sync timestamps can be sorted independently
of packet order. The reader opens without scanning media Clusters and video seeks start at a sync Block.
Sequential inputs report `IndexUnavailable` for seeking. [Matroska timing and indexing](https://www.rfc-editor.org/rfc/rfc9559.html)

### AV1 packet copy

MP4 uses the `av01` sample entry, `av1C` configuration, and `av01` compatible brand. Matroska and WebM use `V_AV1`
and the same configuration record in `CodecPrivate`. Pass the `MediaSampleDescription`, complete temporal units,
and timing through the muxer. AV1 output uses regular or fast-start MP4, or the EBML layouts above. Writers check
marked sync packets before building seek entries. Decoder availability is independent of packet copy.
[AV1 in MP4](https://aomediacodec.github.io/av1-isobmff/) · [Matroska codec mappings](https://www.matroska.org/technical/codec_specs.html)

### Matroska subtitle and metadata copy

Matroska subtitle tracks expose their `S_` CodecID in `MediaTrack::matroska` and use `ContainerSample` framing.
Copy their original codec private bytes and timed packets, including `ContainerPacket::matroska` for the invisible
flag and BlockAdditions. Track attributes retain TrackUID, names, languages, selection flags, attachment links, and
subtitle content encodings. The preview selects audio and video tracks. A subtitle with changing Block codec state
or a custom track clock has no supported copy mapping.

For a full Matroska copy, call `ReadMatroskaMetadata()` on the random-access input and pass its result as
`MediaMuxerOptions::matroska_metadata` with the original tracks. The query reads the title, Segment UID, Chapters,
Tags, and Attachments, including subtitle fonts. It uses SeekHead to locate metadata when the index covers Info and
Tracks. Otherwise it scans a bounded number of element headers and skips sized Cluster payloads. It preserves the
input position and retains up to 64 MiB of element payloads. TrackUIDs and the original metadata
keep chapter and tag associations intact. The writer places these elements before the first Cluster so players
can discover them at open. [Matroska element definitions](https://www.matroska.org/technical/elements.html)

## Write and finish

Pass the same tracks and `MediaMuxerOptions` to `ValidateOutput()` and `Open()`. Validate before creating or truncating the destination. A `SequentialWrite` requirement accepts a
fresh `OutputStream` or an empty writable `RandomAccessStream`. `RandomWrite` requires the latter.
`ContainerLayout::FastStart` is a regular MP4 output layout. It returns `RandomReadWrite` and
places the movie box before media data at `Finish()`, using bounded copying on the supplied stream.
Readers report this as a regular MP4 layout. The actual
open and subsequent I/O can still report SDK errors, including permissions or insufficient space.

For MP4 and MOV, choose an exact output clock using the track's `timescale`. MP4 defaults to the audio sample rate or
90 kHz for video. Supply PTS and duration, and explicit DTS for AVC, HEVC, and AV1. AAC and ProRes decode
time can use PTS. An omitted AAC duration is one 1024-sample access unit. Each track's decode times
must follow a contiguous grid. Presentation times may move backwards across successive packets with
B pictures. WAVE, ADTS, and MOV PCM can derive timing from sample counts and validate any supplied
values against that grid.

Matroska and WebM use a shared Segment tick, independent of `MediaTrack::timescale`. Set
`MediaMuxerOptions::ebml_timestamp_scale` to the desired nanoseconds per tick. The default is 1 ms.
For packet copy, pass `MediaDemuxer::EbmlTimestampScale()` to that option and retain each track's
`default_duration`. This preserves Block times on the source grid and implicit durations even when those
durations are not whole ticks. When separating laced packets, choose a finer tick if their derived starts
fall between the source ticks. Times between ticks round to the nearest tick, with ties away from zero.
Explicit BlockDuration values round their absolute endpoints. Cues and Blocks share the Segment tick.
Audio keeps its codec sample clock, for example when copying compatible AAC to M4A.
[Matroska timestamp ticks](https://www.rfc-editor.org/rfc/rfc9559.html#section-11.1)

The writer preserves decode order per track. WebM also requires nondecreasing stored Block times. A fine grid
can require more Clusters because relative Block timestamps are signed 16-bit values. Output uses a single
Segment and one packet per Block. `Finish()` writes video sync Cues or Cues for audio-only output.

To copy VP8 or VP9 with Opus between WebM and MKV, pass the tracks, packet timing, sync marks, and signed trim
to the destination muxer. `ValidateOutput()` checks the requested mapping. The Frames example reopens
the output. The [preview integration tests](../tests/container_preview.cpp) compare packets per track in both directions.

`priming_samples` and OpusHead must describe the initial delay before opening the writer.
Supply packet trim through `discard_padding` before submitting the affected packet. Positive values
trim the end, and negative values trim the beginning. The trim must fit both the packet's coded duration
and any explicit BlockDuration on the output time grid. Matroska and WebM report `Unsupported` for
nonzero trim supplied only at `EndTrack()`. Other current output formats reject nonzero packet
discard padding or track seek preroll when the mapping cannot represent them.

`TryWrite()` consumes the whole packet when it returns success with no ID. If it returns an ID, supply
that track's next packet or call `EndTrack()` when the track is exhausted, then retry the unconsumed packet.
The MP4 interleaver keeps one bounded packet slot per track. `EndTrack()` can also supply known leading and
trailing audio trim. For example, map `AudioEncodeFormat` to a track, write its `EncodedPacket` values
with the corresponding track ID, then pass the known trim from `AudioEncodeSummary` to `EndTrack()`.

A successful `Finish()` writes the final container information and closes the SDK output. Use `Close()`
or let the session be destroyed to abandon unfinished output. Applications own publication and cleanup of
partial files. When a destination only exposes `AsyncOutputStream`, an application can finish the
container in a writable temporary file and copy it through the SDK's asynchronous streams.

The current bounds include eight tracks, 32 sample descriptions per MP4 or MOV track, 256 edits per track, 16 MiB per
packet, 64 MiB per metadata box, and one million indexed MP4 packets. The Matroska and WebM Cue table and the WebM Block scan each
have a one-million-entry limit. A QuickTime PCM index entry can cover many
sample frames. See [the container contract test](../tests/container_contract.cpp) for codec packet round trips and
[the container fixtures](../tests/fixtures/container/README.md) for independent input samples.
