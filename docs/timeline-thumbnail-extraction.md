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

## Texture output

Use `VideoDecoder::Create(config)` and read its format, frame, and EOS events with `TryRead()`. Use `frame.pts`,
`frame.duration`, and the track's edits to select the frame for a presentation time. Keep one frame for lookahead if
needed. Release unselected frames promptly to free their decoder reservations.

Apply the track's display rotation and pixel aspect ratio to the selected frame. Intersect any container clean
aperture with its decoded visible rectangle. Convert the selected frame with the sized overload of `MakeExternalTexture`.

```cpp
const auto pts = frame.pts;
auto result = MakeExternalTexture(frame, {
    .width = 320,
    .height = 240,
    .fit = ImageFit::Cover,
});
```

The target size is in physical pixels. `Contain` centers the complete image with opaque black padding, and `Cover`
center-crops to fill. Both account for visible geometry, pixel aspect, and rotation. Conversion is synchronous and
accepts eight-bit SDR frames. Keep the cache bounded because a texture that directly retains a decoder image can also
retain an output credit. On Web, handle the frame and texture on the browser main thread.

| Platform | Selected picture conversion |
| --- | --- |
| Android | MediaCodec delivers platform frames through the session's `AImageReader` Surface. A frame retains its `AHardwareBuffer` until release. Only a selected frame passed to `MakeExternalTexture` is imported and sampled by GLES into a small RGBA target. SDK publication copies that small target on the GPU. |
| macOS | Retain the original pixel buffer for matching geometry when its attachments allow it. Otherwise Core Image uses Metal to render into a small BGRA pixel buffer. |
| iOS | The iOS backend independently uses the same Core Image and pixel-buffer publication contract. |
| Windows | The D3D11 video processor renders the selected picture directly to a small BGRA texture. The SDK makes its shared GPU snapshot at that size. A CPU decoder configuration produces a software `PixelTexture`. |
| Linux | GStreamer supplies CPU YUV. Convert and scale only selected pictures to RGBA, and retain the result through the SDK's GDK texture wrapper. DMA-BUF decoding and GPU scaling need a separate platform backend. |
| Web | Retain the original browser frame for matching geometry. Otherwise draw to a target-sized canvas and publish its `VideoFrame`. The application does not read back the pixels of a browser frame. |

Completed textures survive subsequent output, reset, and close. The `media_video_texture_selection` regression checks
PTS and physical dimensions across reset. On Android it also checks that reading and discarding frames issues no GL
draw and that texture creation uses no CPU pixel readback.
