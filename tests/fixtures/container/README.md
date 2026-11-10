# Synthetic container fixtures

These files use generated test patterns and sine waves. FFmpeg 6.1.1 produced the original files.
FFmpeg is a fixture-generation and interoperability tool for these tests.

- `hevc-aac-portrait.mov` carries six HEVC frames, mono AAC-LC, and a clockwise 90-degree track
  matrix. The HEVC track uses the `hvc1` sample entry and the video packets include reordering.
- `prores-pcm.mov` carries four ProRes 422 frames, 4000 stereo S24LE PCM sample frames, and an
  auxiliary timecode track. The audio uses QuickTime's version-one `in24` sample description.
- `mkv-hevc-aac.mkv` remuxes the HEVC and AAC fixture with reordered video and Cues.
- `mkv-avc-aac.mkv` carries H.264 Main with reordered video, mono AAC-LC, and Cues.
- `webm-vp8-opus.webm` carries VP8 and Opus with Cues and final discard padding.
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
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=0.75 \
  -f lavfi -i sine=sample_rate=48000:duration=0.75 -c:v libx264 -profile:v main \
  -pix_fmt yuv420p -bf 2 -g 6 -c:a aac -b:a 32k mkv-avc-aac.mkv
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=1.25 \
  -f lavfi -i sine=sample_rate=48000:duration=1.25 -c:v libvpx -b:v 40k \
  -g 6 -lag-in-frames 0 -c:a libopus -b:a 32k webm-vp8-opus.webm
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=1.25 \
  -f lavfi -i sine=sample_rate=48000:duration=1.25 -c:v libvpx-vp9 -b:v 0 -crf 36 \
  -g 6 -lag-in-frames 0 -c:a libopus -b:a 32k webm-vp9-opus.webm
ffmpeg -f lavfi -i sine=sample_rate=48000:duration=5.15 \
  -c:a libopus -b:a 16k -cluster_time_limit 250 webm-opus.webm
```

## Real-container preview and audio formats

- `preview-avc-aac.mp4` carries four seconds of generated color and audio markers, with Main-profile AVC
  B pictures, uneven presentation intervals, and AAC priming. It is the bundled Frames sample.
- `preview-hevc10.mov` is an AVAssetWriter-generated Main10 `hvc1` movie with an SDR BT.709 ramp and
  a clockwise 90-degree transform. Generate it on macOS with
  `xcrun swift generate_hevc10.swift preview-hevc10.mov` in this directory.
- `preview-hevc-hlg.mov` uses the same generated ramp and transform with BT.2020 HLG, 4:3 pixel aspect,
  and the `dvvC` payload emitted by AVAssetWriter. It exercises metadata and exact access-unit retention on
  the packet-copy path. Generate it with `xcrun swift generate_hevc10.swift preview-hevc-hlg.mov hlg`.
- An independent fragmented MP4 for device checks can be generated from the bundled AVC/AAC fixture with
  `ffmpeg -i preview-avc-aac.mp4 -c copy -movflags +empty_moov+frag_keyframe+default_base_moof fragmented.mp4`.
- `preview-opus.ogg`, `preview-flac.flac`, and `preview-mp3.mp3` use generated sine waves.
  The Ogg end granule is 107496 at 48 kHz with a pre-skip of 120 samples. FLAC and MP3 both represent
  50142 audible samples at 44.1 kHz. The MP3 uses the LAME VBR header.

```sh
ffmpeg -f lavfi -i sine=sample_rate=48000:duration=2.237 -c:a opus -strict experimental -ac 2 -b:a 48k preview-opus.ogg
ffmpeg -f lavfi -i sine=sample_rate=44100:duration=1.137 -c:a flac -ac 2 preview-flac.flac
ffmpeg -f lavfi -i sine=sample_rate=44100:duration=1.137 -c:a pcm_s16le -ac 2 audio.wav
lame -V 4 audio.wav preview-mp3.mp3
```

```sh
ffmpeg -f lavfi -i 'testsrc2=size=320x240:rate=24:duration=4' \
  -f lavfi -i 'aevalsrc=0.12*sin(2*PI*1000*t)*gte(mod(n\,48000)\,24000)*lt(mod(n\,48000)\,28000):s=48000:d=4' \
  -vf "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='between(mod(n,24),12,13)',select='not(eq(mod(n,11),5))'" \
  -vsync vfr -pix_fmt yuv420p -colorspace bt709 -color_range tv \
  -c:v libx264 -profile:v main -bf 2 -g 22 -video_track_timescale 24000 \
  -c:a aac -profile:a aac_low -ac 2 -b:a 64k preview-avc-aac.mp4
```
