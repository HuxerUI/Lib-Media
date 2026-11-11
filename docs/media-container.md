# Media containers

Include `<huxerui/media_container.h>` and link `HuxerUI::Media`. The two move-only sessions work
with the SDK's synchronous streams and can run on a caller-owned worker without application startup.
Use an SDK build that provides `RandomAccessStream` and `File::OpenRandomAccess()`.
Serialize calls on each session.

`MediaTrack::codec` uses `CodecId` from `<huxerui/media_format.h>`. Pass that identifier and the
track's initialization data to the corresponding decoder configuration for a supported mapping.

## Supported combinations

| Container | Packets | Input | Output |
| --- | --- | --- | --- |
| Regular MP4, including audio-only M4A | Raw mono/stereo AAC-LC or ALAC, and length-prefixed AVC or HEVC | Random access, or sequential when movie metadata precedes samples | Random write; fast-start requires random read/write |
| Fragmented MP4 | AAC, AVC, and HEVC | Sequential from initialization. Sized random-access input supports incremental seek, including growing files. | Sequential or random write |
| QuickTime MOV | AAC, AVC, HEVC, ProRes, packed interleaved PCM, timecode, and boxed timed metadata | Random access, or sequential when movie metadata precedes samples | Random write |
| Matroska, including audio-only MKA | Length-prefixed AVC or HEVC, raw AAC-LC, and mapping-family-zero Opus | Sequential or random access | Sequential or random write |
| WebM | VP8, VP9, and mapping-family-zero Opus | Sequential or random access | Sequential or random write |
| RIFF WAVE and RF64 | Interleaved U8, S16LE, S24LE, S32LE, or F32LE PCM | Sequential or random access | Random write |
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

The audio-copy example also copies fragmented AAC to regular M4A. It collects the packet presentation interval
as it copies and applies any supported track edit before checking the finished file. Native M4A copies place
the index before the samples. On Web, the example keeps the index after the samples so finalization does not
relocate the media data. The Web output can be reopened with random access.

ALAC uses `CodecId::Alac`, mono or stereo, and the 24-byte `ALACSpecificConfig` in `initialization_data`.
Write each original packet with its actual duration, including a shorter final packet. The packet-copy path
retains the lossless bitstream and sample clock. PCM playback and encoding in the current audio codec API
use other supported mappings, such as AAC-LC.

For a single audio track, `audio_metadata` carries title, artist, album, track index and total, genre, date,
and the first JPEG or PNG cover. Regular and fast-start MP4 write these fields once in `udta/meta/ilst`.
Supply the tags on the first sample description when opening a writer with multiple AAC configurations.
`SampleDescriptions()` returns the tags on the first entry, and the current track description retains them
across configuration changes. An empty optional metadata box leaves the audio track and its packets readable without tags.
[QuickTime metadata item list](https://developer.apple.com/documentation/quicktime-file-format/metadata_item_list_atom)

### Track presentation intervals

`MediaDemuxer::PresentationRange(track_id)` returns the half-open bounds of the presented samples on the common
timeline. The bounds can include gaps between edit segments. Equal endpoints describe an empty interval. An absent result means coverage
is unavailable or the track ID is unknown. The query does not move the cursor or build an index. `Read()` continues
to return coded preroll and tail packets. Apply the range to decoded pictures and PCM for presentation.
Native FLAC and MP3 provide a range once their total sample count is known, with any recognized gapless trim applied.

`MediaTrack::presentation_edit` carries a finite authored interval for one rate-one edit with an optional leading
empty edit. The reader normalizes its packet timestamps onto the common clock. Audio trim is relative to packet
timing, so shortened sample durations already account for those samples. A fragmented writer needs the interval at
open to declare AAC priming and tail trim. Any separately supplied trim must agree with the interval and packet
timeline. A sequential writer can confirm the declared trim at `EndTrack()`.

`MediaTrack::edits` carries additional rate-one edit segments. An empty entry advances presentation without
selecting media. Media intervals are forward and nonoverlapping. Packet PTS and DTS remain on the track's media
clock. Intersect decoded frame intervals with each selected media interval and map them to the accumulated
presentation position. Seek queries take presentation time and resolve the corresponding media time and decode
anchor. Pass the edit list with the original packets to an MP4 or MOV writer to retain the cuts and gaps.
Regular output rebases the media clock and edit offsets together when the first DTS is nonzero.
`Duration()` includes declared trailing empty edits. `PresentationRange()` ends at the last presented sample.

### Sample descriptions

`SampleDescriptions(track_id)` exposes the available descriptions. MP4 and MOV accept multiple descriptions of
one codec for AVC, HEVC, and AAC. AAC descriptions share a sample rate and channel count. Read the
`MediaTrack` event before submitting its packets to a decoder. Reconfigure at a video sync picture when the
configuration changes. Pass the descriptions with their consecutive one-based indices to the muxer at open.
`ContainerPacket::sample_description_index` selects the matching description in the sample table or fragment.

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

For WAVE, an extensible track carries the 22-byte format suffix in `initialization_data`. It preserves
the subtype, valid bits, and speaker mask when passed to a WAVE or RF64 writer. WAVE packets are
frame-aligned blocks. ADTS strips transport headers on read and supplies them on write. It provides a
sample-count timeline starting at zero and emits an updated track description if the AAC configuration changes.

The container API covers the track and packet fields declared in the header. Tag and cover support includes
M4A, native FLAC, and MP3. The current mappings cover the documented track types, visual metadata, and rate-one edits.
Mirrored, scaled, or perspective movie and track matrices report `Unsupported`.
A format or codec combination outside the table returns a format or support error.

Ogg Opus preserves OpusHead, pre-skip, and final-packet trim. The reader validates page CRCs and
assembles continued packets. At a chain boundary it emits an updated `MediaTrack` before the new link's
packets. Each link begins at the previous link's audible end. Its leading packets can precede that point
on the coded timeline. Random input builds a bounded link and page index on the first seek query. The
80 ms Opus preroll stays within the selected link. After `Seek()`, use the replayed description to open or
reset the decoder for that link. The writer emits one packet per page for a single logical stream and
accepts up to 1 ms of timestamp rounding for WebM interoperability. Supported streams use mapping family zero.
[Ogg Opus mapping and seeking](https://www.rfc-editor.org/rfc/rfc7845.html#section-4.6)

Native FLAC validates frame geometry, position, and checksums. Random input uses checked `SEEKTABLE`
entries when present, with a bounded frame index as a fallback. The writer retains the compressed frames,
updates STREAMINFO sample and frame-size statistics, and writes a bounded seek table. Frame numbering and
timing must begin at the stream origin. The STREAMINFO PCM checksum belongs to the supplied complete stream.
[FLAC metadata and frame format](https://www.rfc-editor.org/rfc/rfc9639.html#section-8)

MP3 handles MPEG-1, MPEG-2 and MPEG-2.5 Layer III with a fixed sample rate and layout. The reader understands
Xing/Info and VBRI frame counts and recognized LAME delay and padding. It builds a bounded frame index and
returns an earlier reservoir restart point as `decode_start`. The writer requires a restartable first frame,
copies the original audio bytes, and rebuilds Xing/Info frame and byte counts, its seek table, and supplied
gapless information. The demuxer's audio window describes the encoder trim on the coded timeline.

Native FLAC and MP3 expose common text and retained JPEG or PNG front-cover bytes through
`MediaTrack::audio_metadata`. Supported text fields are title, artist, album, track number, genre, and date.
FLAC uses Vorbis comments and PICTURE. MP3 reads ID3v2.3 and v2.4 and writes ID3v2.4. Copying a track to its
native format rewrites these fields without modifying the compressed audio. The output mapping supports one
track and requires a seekable destination for final metadata. [ID3 frame definitions](https://id3.org/id3v2.4.0-frames)

## Read and seek

Open a `File` or `FileReference` through the SDK. Choose `OpenRandomAccess()` for positioning or for
regular MP4 with metadata after media data. The session takes ownership of the stream, including on open failure.
The SDK determines which file providers expose synchronous streams.

`Tracks()` provides a borrowed view of the known descriptions. `Read()` yields a packet or an updated description.
Use the most recent description for that track before interpreting its packet. A successful empty result means
EOF for a fixed input. Truncated packets and malformed sample tables are errors. A returned `BufferReference` keeps its bytes alive
across reads, seeks, and session closure.

`FindSeekPoint()` preserves the read position. `Seek()` uses indexed anchors in MP4, MOV, Matroska, and WebM,
sample frames in WAVE and RF64, or packet anchors in Ogg, native FLAC, and MP3. Check the returned point and its
evidence before starting a decoder. A reference-track anchor can also affect the starting positions of other
selected tracks. The first packet read from each track after a seek is preceded by its description.

Fixed fMP4 can be read from the start one fragment batch at a time. On sized random-access inputs, a batch can
include several fragments that share media data. Their sample ranges are checked against complete `mdat` boxes.
Fixed inputs establish the final duration and trailing audio trim at EOF. Preview and export read packets as needed.

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
AVC carries the AVCDecoderConfigurationRecord in CodecPrivate. VP8 uses complete codec packets
without CodecPrivate. Use container sync marks when submitting VP8 or VP9 for indexed output.

Matroska Block time is a presentation clock adjusted by the track's CodecDelay. The reader leaves DTS absent.
BlockDuration, DefaultDuration, and known codec frame durations supply timing where available. Opus preserves
OpusHead and pre-skip on its 48 kHz decoded sample grid. `seek_preroll` and `discard_padding` retain
the track preroll and signed per-packet trim.

Random input uses Cues from SeekHead or finds existing tail Cues by following EBML element boundaries.
Each returned anchor is matched to a Block on its reference track. An Opus seek can start before the
anchor at the returned `decode_start`. A file with no usable Cues for that track reports
`IndexUnavailable` for seek. Sequential preview and WAV export can continue using the packet timestamps and trim.

## Write and finish

Call `ValidateOutput()` before creating or truncating the destination. A `SequentialWrite` requirement accepts a
fresh `OutputStream` or an empty writable `RandomAccessStream`. `RandomWrite` requires the latter.
`ContainerLayout::FastStart` is a regular MP4 output layout. It returns `RandomReadWrite` and
places the movie box before media data at `Finish()`, using bounded copying on the supplied stream.
Readers report this as a regular MP4 layout. The actual
open and subsequent I/O can still report SDK errors, including permissions or insufficient space.

Choose an exact output clock using the track's `timescale`. MP4 defaults to the audio sample rate or
90 kHz for video. Supply PTS and duration, and explicit DTS for AVC and HEVC. AAC and ProRes decode
time can use PTS. An omitted AAC duration is one 1024-sample access unit. Each track's decode times
must follow a contiguous grid. Presentation times may move backwards across successive packets with
B pictures. WAVE, ADTS, and MOV PCM can derive timing from sample counts and validate any supplied
values against that grid.

Matroska and WebM use a 1 ms output timestamp grid. The writer keeps the supplied decode order per track
and rounds absolute packet times and duration boundaries. WebM additionally requires nondecreasing
stored Block times. Output uses a single Segment, bounded Clusters, and one packet per Block. Finish
writes available video sync Cues or audio Cues for audio-only output. Output duration is absent from
the initial header.

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
packet, 64 MiB per metadata box, and one million indexed MP4 packets. The Matroska and WebM Cue table also
uses a one-million-entry limit. A QuickTime PCM index entry can cover many
sample frames. See [the container contract test](../tests/container_contract.cpp) for codec packet round trips and
[the container fixtures](../tests/fixtures/container/README.md) for independent input samples.
