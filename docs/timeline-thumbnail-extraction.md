# Timeline thumbnail extraction

Lib-AVKit chooses the thumbnail times for the visible timeline range, schedules extraction, and owns the
cache. Lib-Media supplies packets, seek points, and decoded video frames.

## Sequential decoding and GOP skips

Generate ordered targets at `N` thumbnails per second on an exact time grid. Map project times to the source
track's presentation timeline. Start producing the first requested thumbnails as soon as the reader and
decoder are ready. A request that starts partway into a clip can seek to its first target's decode anchor.

While the first thumbnails are being produced, prefetch anchors for upcoming targets with
`FindSeekPoint(track_id, target, SeekDirection::AtOrBefore)`. Run concurrent lookups on a separate demuxer
with its own stream. Calls on an individual demuxer are serialized, and a lookup can extend a fragmented MP4
index. Cache the anchors for reuse as the viewport moves. [Container API](../include/huxerui/media_container.h)

Keep the decoder running for targets in the same GOP. If an `Independent` anchor lies ahead of the packets
already submitted and skipping to it saves enough work to justify the seek and decoder reset, jump to it and
decode forward. Reset the existing decoder if the configuration matches, or recreate it when the coded size
or initialization data changes. Select the thumbnail from decoded presentation times and apply track edits
when mapping frames back to the target timeline.

Cache the thumbnail by source, track, target time, output pixel size, and any applied crop or rotation. Record
the actual frame PTS so a different timeline zoom can reuse the same image when its target selects that frame.

## Android GPU snapshots

The Android path sends selected MediaCodec outputs to a `SurfaceStreamTexture`. HuxerUI samples the OES
image with its SurfaceTexture transform and draws directly into a thumbnail-sized `GlFrame` on the GPU. Pass
the coded video dimensions to the decoder and the thumbnail pixel dimensions to `SurfaceStreamTexture::Create()`.
The SDK uses the latter for the published snapshot and the requested Surface default buffer size. Android
video producers can override that default buffer size.
[Android SurfaceTexture](https://developer.android.com/reference/android/graphics/SurfaceTexture#setDefaultBufferSize(int,int))

Lib-Media's Android preview already chooses which decoder outputs reach the Surface. It currently creates
the texture at the coded dimensions. The thumbnail path should accept the target pixel dimensions and provide
Lib-AVKit with the selected frame's completed snapshot. This uses the SDK's existing OES conversion and avoids
a full-size RGBA snapshot before scaling. [Android preview](gpu-video.md#preview-and-verification)

Allow one selected frame to await publication on a thumbnail surface at a time. Once it publishes, retain the
immutable `GlFrame` from `AcquireFrame()` and associate it with the selected decoder output's PTS. Complete or
retire pending work before associating a new result after a seek or source change. `SurfaceTexture` latches the
latest available image, so this ordering matters for exact thumbnails.
[Android frame latching](https://developer.android.com/reference/android/graphics/SurfaceTexture#updateTexImage())

For an initial Android check, compare sparse and dense thumbnail requests on a long-GOP SDR H.264 MP4 with a
sequential decode of the same source. Check the selected frame times, snapshot dimensions, retention across
later publications, and absence of CPU pixel readback. Measure time to first thumbnail and the number of
packets submitted to the decoder.
