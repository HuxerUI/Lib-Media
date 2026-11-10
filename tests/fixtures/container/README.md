# Synthetic container fixtures

These files use generated test patterns and sine waves. FFmpeg 6.1.1 produced the original files.
FFmpeg is a fixture-generation and interoperability tool for these tests.

- `hevc-aac-portrait.mov` carries six HEVC frames, mono AAC-LC, and a clockwise 90-degree track
  matrix. The HEVC track uses the `hvc1` sample entry and the video packets include reordering.
- `prores-pcm.mov` carries four ProRes 422 frames, 4000 stereo S24LE PCM sample frames, and an
  auxiliary timecode track. The audio uses QuickTime's version-one `in24` sample description.
- `mkv-hevc-aac.mkv` remuxes the HEVC and AAC fixture with reordered video and Cues.
- `webm-vp9-opus.webm` carries VP9 and Opus with pre-skip, seek preroll, and final discard padding.
- `webm-opus.webm` carries audio-only Opus with frequent Cues for testing preroll across Cluster boundaries.

```sh
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=0.5 \
  -f lavfi -i sine=sample_rate=48000:duration=0.5 -c:v libx265 -preset ultrafast \
  -pix_fmt yuv420p -x265-params pools=1:frame-threads=1:keyint=3:min-keyint=3:bframes=1 \
  -tag:v hvc1 -c:a aac hevc-source.mov
ffmpeg -display_rotation:v:0 -90 -i hevc-source.mov -c copy hevc-aac-portrait.mov
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=8:duration=0.5 \
  -f lavfi -i sine=sample_rate=8000:duration=0.5 -c:v prores_ks -profile:v 2 \
  -pix_fmt yuv422p10le -c:a pcm_s24le -ac 2 -timecode 01:00:00:00 prores-pcm.mov
ffmpeg -i hevc-aac-portrait.mov -map 0:v:0 -map 0:a:0 -c copy -map_metadata -1 mkv-hevc-aac.mkv
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=1.25 \
  -f lavfi -i sine=sample_rate=48000:duration=1.25 -c:v libvpx-vp9 -b:v 0 -crf 36 \
  -g 6 -lag-in-frames 0 -c:a libopus -b:a 32k webm-vp9-opus.webm
ffmpeg -f lavfi -i sine=sample_rate=48000:duration=5.15 \
  -c:a libopus -b:a 16k -cluster_time_limit 250 webm-opus.webm
```
