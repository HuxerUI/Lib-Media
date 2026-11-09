# Media containers

Include `<huxerui/media_container.h>` and link `HuxerUI::Media`. The two move-only sessions work
with the SDK's synchronous streams and can run on a caller-owned worker without application startup.
Use an SDK build that provides `RandomAccessStream` and `File::OpenRandomAccess()`.
Serialize calls on each session.

## Supported combinations

| Container | Packets | Input | Output |
| --- | --- | --- | --- |
| Regular MP4, including audio-only M4A | Raw mono/stereo AAC-LC and length-prefixed AVC or HEVC | Random access, or sequential when movie metadata precedes samples | Random write |
| Fragmented MP4 | Same AAC, AVC, and HEVC mappings | Sequential or random-access input, beginning with initialization | Sequential or random write |
| QuickTime MOV | Same AAC, AVC, and HEVC mappings, ProRes, and packed interleaved PCM | Random access, or sequential when movie metadata precedes samples | Random write |
| RIFF WAVE and RF64 | Interleaved U8, S16LE, S24LE, S32LE, or F32LE PCM | Sequential or random access | Random write |
| MPEG-4 ADTS | One raw mono/stereo AAC-LC access unit per frame | Sequential or random access | Sequential or random write |

MP4 supports one description per track and self-contained samples in decode order within each track.
Regular MP4 accepts one rate-one edit and an optional leading empty edit. Its writer uses those edits to
preserve track offsets and audio trim. Trim is measured relative to packet timing from the sample tables,
so a shortened sample duration is already reflected in that timeline. The fragmented writer emits an initialization movie and bounded
fragments. It requires nonnegative DTS and rejects nonzero audio trim. Fragmented MP4 and ADTS
currently report `IndexUnavailable` for time-based index queries.

HEVC tracks retain `hvcC` and their `hvc1` or `hev1` sample entry. With `hvc1`, parameter sets are
in the configuration and the writer rejects in-band parameter sets. `hev1` can carry parameter sets in
samples, and its seek points report the container sync mark. With random input, a validated `hvc1`
IDR can report `Independent`. Presentation rotation combines right-angle movie and track transforms
and is emitted as a track matrix. Packet bytes and coded dimensions retain their original orientation.

MOV uses the `qt  ` major brand and self-contained media. ProRes retains the Proxy, LT, 422, HQ,
4444, or 4444 XQ sample entry. PCM supports U8, signed 16/24/32-bit, and 32-bit float, with little-
and big-endian variants where applicable. The reader groups consecutive PCM frames in each chunk.
The writer emits packed interleaved LPCM in a version-two sound description. MOV input can include
auxiliary timecode and timed metadata tracks. The session reads and writes the supported audio and
video tracks, and skips those auxiliary tracks.

For WAVE, an extensible track carries the 22-byte format suffix in `initialization_data`. It preserves
the subtype, valid bits, and speaker mask when passed to a WAVE or RF64 writer. WAVE packets are
frame-aligned blocks. ADTS strips transport headers on read and supplies them on write. It provides a
sample-count timeline starting at zero and emits an updated track description if the AAC configuration changes.

The container API covers the track and packet fields declared in the header. Container-specific metadata
such as tags, chapters, attachments, color metadata, and complex edit lists is outside these mappings.
Mirrored, scaled, or perspective movie and track matrices report `Unsupported`.
A format or codec combination outside the table returns a format or support error.

## Read and seek

Open a `File` or `FileReference` through the SDK. Choose `OpenRandomAccess()` for positioning or for
regular MP4 with metadata after media data. The session takes ownership of the stream, including on open failure.
The SDK determines which file providers expose synchronous streams.

`Tracks()` provides a borrowed view of the known descriptions. `Read()` yields a packet or an updated description.
Use the most recent description for that track before interpreting its packet. A successful empty result means
EOF. Truncated packets and malformed sample tables are errors. A returned `BufferReference` keeps its bytes alive
across reads, seeks, and session closure.

`FindSeekPoint()` preserves the read position. `Seek()` places subsequent reads at the selected anchor in regular
MP4 or at a sample frame in WAVE. Check the returned point and its evidence before starting a decoder. A
reference-track anchor can also affect the starting positions of other selected tracks. The first packet read
from each track after a seek is preceded by its description.

## Write and finish

Call `ValidateOutput()` before creating or truncating the destination. A `SequentialWrite` requirement accepts a
fresh `OutputStream` or an empty writable `RandomAccessStream`. `RandomWrite` requires the latter. The actual
open and subsequent I/O can still report SDK errors, including permissions or insufficient space.

Choose an exact output clock using the track's `timescale`. MP4 defaults to the audio sample rate or
90 kHz for video. Supply PTS and duration, and explicit DTS for AVC and HEVC. AAC and ProRes decode
time can use PTS. An omitted AAC duration is one 1024-sample access unit. Each track's decode times
must follow a contiguous grid. Presentation times may move backwards across successive packets with
B pictures. WAVE, ADTS, and MOV PCM can derive timing from sample counts and validate any supplied
values against that grid.

`TryWrite()` consumes the whole packet when it returns success with no ID. If it returns an ID, supply
that track's next packet or call `EndTrack()` when the track is exhausted, then retry the unconsumed packet.
The MP4 interleaver keeps one bounded packet slot per track. `EndTrack()` can also supply known leading and
trailing audio trim. For example, map `AudioEncodeFormat` to a track, write its `EncodedPacket` values
with the corresponding track ID, then pass the known trim from `AudioEncodeSummary` to `EndTrack()`.

A successful `Finish()` writes the final container information and closes the SDK output. Use `Close()`
or let the session be destroyed to abandon unfinished output. Applications own publication and cleanup of
partial files. When a destination only exposes `AsyncOutputStream`, an application can finish the
container in a writable temporary file and copy it through the SDK's asynchronous streams.

The current bounds include eight tracks, 16 MiB per packet, 64 MiB per metadata box, and one million
indexed packets for a regular movie or an individual fragment. A QuickTime PCM index entry can cover many
sample frames. See [the container contract test](../tests/container_contract.cpp) for codec packet round trips and
[the QuickTime fixtures](../tests/fixtures/container/README.md) for independent input samples.
