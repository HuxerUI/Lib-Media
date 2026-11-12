# Synthetic container fixtures

These files use generated test patterns and sine waves. FFmpeg 6.1.1 produced the original files.
FFmpeg is a fixture-generation and interoperability tool for these tests.

- `hevc-aac-portrait.mov` carries six HEVC frames, mono AAC-LC, and a clockwise 90-degree track
  matrix. The HEVC track uses the `hvc1` sample entry and the video packets include reordering.
- `prores-pcm.mov` carries four ProRes 422 frames, 4000 stereo S24LE PCM sample frames, and an
  auxiliary timecode track. The audio uses QuickTime's version-one `in24` sample description.
- `prores-proxy-pcm.mov`, `prores-lt-pcm.mov`, and `prores-hq-pcm.mov` use `apco`, `apcs`, and `apch`.
  Each has four pictures with alternating ten-bit chroma rows and 4000 stereo S24LE PCM sample frames.
  FFmpeg n4.1.3-381 generated these fixtures. The HQ fixture exercises 4:2:2 transfer to the standard 422 encoder.
- `mkv-hevc-aac.mkv` remuxes the HEVC and AAC fixture with reordered video and Cues.
- `mkv-avc-aac.mkv` carries H.264 Main with reordered video, mono AAC-LC, and Cues.
- `webm-vp8-opus.webm` carries VP8 and Opus with Cues and final discard padding.
- `webm-vp9-opus.webm` carries VP9 and Opus with pre-skip, seek preroll, and final discard padding.
- `preview-vp9-opus-no-cues.webm` carries 18 profile 0 VP9 pictures at 320x240 and 72000 audible Opus
  samples. FFmpeg 9.0 wrote it through a pipe with no Cues or declared duration. Its last Opus Block
  carries both the rounded audible BlockDuration and 648 samples of discard padding.
- `preview-av1.webm`, `preview-av1.mkv`, and `preview-av1.mp4` contain the same 24 Main 8-bit AV1 pictures at
  320x240 and 15 fps. FFmpeg 9.0 encoded the test pattern with `libaom-av1`, CRF 40, a key interval of eight,
  and eight frames of lookahead. The audio is a 600 Hz mono tone at 48 kHz in Opus for EBML and AAC-LC for MP4.
  The MP4 repeats the sequence header in the second sync sample to exercise seek and copy with a valid repeated header.
  The MKV additionally has two SubRip captions in an English default/forced track, two titled chapters,
  global and track tags, and a text attachment. The preview test seeks across GOPs and checks all packet bytes,
  timing, and retained MKV metadata after copy.
- `webm-opus.webm` carries 247200 audible mono samples at 48 kHz with frequent Cues for testing preroll across
  Cluster boundaries. Its 258 coded packets each represent 960 samples, with 312 initial and 168 trailing samples trimmed.

```sh
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=12:duration=0.5 \
  -f lavfi -i sine=sample_rate=48000:duration=0.5 -c:v libx265 -preset ultrafast \
  -pix_fmt yuv420p -x265-params pools=1:frame-threads=1:keyint=3:min-keyint=3:bframes=1 \
  -tag:v hvc1 -c:a aac hevc-source.mov
ffmpeg -display_rotation:v:0 -90 -i hevc-source.mov -c copy hevc-aac-portrait.mov
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=8:duration=0.5 \
  -f lavfi -i sine=sample_rate=8000:duration=0.5 -c:v prores_ks -profile:v 2 \
  -pix_fmt yuv422p10le -c:a pcm_s24le -ac 2 -timecode 01:00:00:00 prores-pcm.mov
for variant in '0 proxy' '1 lt' '3 hq'; do
  ffmpeg -f lavfi \
    -i "nullsrc=size=64x48:rate=8:duration=0.5,format=yuv422p10le,geq=lum='320+80*N':cb='if(mod(Y,2),704,320)':cr='if(mod(Y,2),384,640)'" \
    -f lavfi -i "aevalsrc=0.2*sin(2*PI*440*t)|0.15*sin(2*PI*660*t):s=8000:d=0.5" \
    -c:v prores_ks -profile:v "${variant%% *}" -pix_fmt yuv422p10le -color_range tv \
    -colorspace bt709 -color_primaries bt709 -color_trc bt709 -c:a pcm_s24le "prores-${variant##* }-pcm.mov"
done
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
ffmpeg -f lavfi -i 'testsrc2=size=320x240:rate=12:duration=1.5' \
  -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1.5' \
  -c:v libvpx-vp9 -g 6 -lag-in-frames 0 -auto-alt-ref 0 -crf 40 -b:v 0 \
  -pix_fmt yuv420p -colorspace bt709 -color_trc bt709 -color_primaries bt709 \
  -c:a libopus -ac 1 -b:a 24k -cluster_time_limit 250 -live 1 -f webm pipe:1 > preview-vp9-opus-no-cues.webm
ffmpeg -f lavfi -i sine=sample_rate=48000:duration=5.15 \
  -c:a libopus -b:a 16k -cluster_time_limit 250 webm-opus.webm
```

## Real-container preview and audio formats

- `preview-avc-aac.mp4` carries four seconds of generated color and audio markers, with Main-profile AVC
  B pictures, uneven presentation intervals, and AAC priming. It is the bundled Frames sample.
- `preview-hevc-main.mp4` has 20 generated SDR Main-profile HEVC pictures with an `hvc1` entry.
  The Web and iOS preview tests encode it and compare every picture timestamp after MP4 readback.
- `preview-hevc10.mov` is an AVAssetWriter-generated Main10 `hvc1` movie with an SDR BT.709 ramp and
  a clockwise 90-degree transform. Generate it on macOS with
  `xcrun swift generate_hevc10.swift preview-hevc10.mov` in this directory.
- `preview-hevc-hlg.mov` uses the same generated ramp and transform with BT.2020 HLG, 4:3 pixel aspect,
  and the `dvvC` payload emitted by AVAssetWriter. It exercises metadata and exact access-unit retention on
  the packet-copy path. Generate it with `xcrun swift generate_hevc10.swift preview-hevc-hlg.mov hlg`.
- `preview-hevc-metadata.mov` uses the SDR ramp with an AVAssetWriter `mebx` video orientation sample and a
  `cdsc` association. Generate it with `xcrun swift generate_hevc10.swift preview-hevc-metadata.mov metadata`.
- An independent fragmented MP4 for device checks can be generated from the bundled AVC/AAC fixture with
  `ffmpeg -i preview-avc-aac.mp4 -c copy -movflags +empty_moov+frag_keyframe+default_base_moof fragmented.mp4`.
- `preview-opus.ogg`, `preview-flac.flac`, and `preview-mp3.mp3` use generated sine waves.
  The Ogg end granule is 107496 at 48 kHz with a pre-skip of 120 samples. FLAC and MP3 both represent
  50142 audible samples at 44.1 kHz. The MP3 uses the LAME VBR header.
- `opus-link.ogg` is a second 48 kHz stereo link with 2.5 ms packets, a 120-sample pre-skip, and 34224
  audible samples. The preview test remuxes it with zero pre-skip, appends it to `preview-opus.ogg`, and verifies
  playback across the link boundary, repeated seeks, and the signal in the first packet of the second link.
- `streaming-opus.webm` has no duration or Cues. It carries 38 960-sample stereo packets at 48 kHz, with
  120 samples of pre-skip and 480 of end trim. The preview test checks sequential playback and 35880 WAV samples.
- `tagged-flac.flac` and `tagged-mp3.mp3` carry the same audio as the untagged samples and contain UTF-8 or
  UTF-16 text and a front cover. FLAC includes a native seek point. The tests compare every compressed frame
  before and after copying and verify that WAV readback contains exactly 50142 samples.
- `tagged-aac.m4a` has AAC-LC at 48 kHz, 1024 samples of encoder priming, and 52800 audible samples. Its last
  packet spans 576 samples on the container timeline. The metadata test replaces its `udta` or `ilst` with
  an empty box and checks that the audio timeline and packets remain readable.
- `tagged-alac.m4a` has three lossless packets at 96 kHz with 10560 samples, including a shorter final packet.
  Both M4A fixtures carry the same text tags and PNG cover and exercise fast-start copy and seek.
- `preview-pcm.aiff` carries 8821 mono S24BE frames at 44.1 kHz and has an odd-length sound payload.
  `preview-pcm.caf` carries 9600 stereo F32BE frames at 48 kHz. Core Audio wrote both files.
- `preview-pcm.w64` has 8821 mono F32LE frames at 44.1 kHz. FFmpeg writes four alignment bytes in its
  declared data length and supplies the exact count in `fact`.
- `preview-vorbis.ogg` carries 97593 stereo PCM samples at 44.1 kHz after Vorbis synthesis and final granule
  trim. It uses short and long blocks across two audio pages. The preview regression verifies PCM after
  backward and forward seeks and checks original packets through WebM and MKA round trips.
- `trimmed-alac.caf` carries stereo ALAC at 96 kHz in packets of 4096, 4096, and 2368 coded samples.
  Its valid interval retains 10540 samples after 7 samples of priming and 13 of tail trim. The fixed-frame
  table includes the 1728 uncoded samples of the last nominal packet in its remainder.
- `alac-edit-8821.m4a` and `alac-edit-8813.m4a` carry mono ALAC at 44.1 kHz with 8821 and 8813 samples.
  Their FFmpeg edit lengths are 201 and 200 ms respectively. Both extend beyond the last packet, and the 201 ms
  boundary also falls between samples. The regression compares every packet and checks the valid CAF sample count.
- `fragmented-aac.m4a` has 23 AAC-LC packets over four fragments, spanning 22624 samples at 48 kHz. The
  preview test copies it to regular M4A and compares packet contents and timing with indexed source readback.

```sh
ffmpeg -f lavfi -i 'testsrc2=size=320x240:rate=30:duration=0.64' -c:v hevc_videotoolbox \
  -profile:v main -pix_fmt nv12 -tag:v hvc1 -g 6 -bf 0 -b:v 500k -movflags +faststart preview-hevc-main.mp4
ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=0.45' -c:a aac -ac 2 \
  -movflags +empty_moov+default_base_moof+frag_keyframe -frag_duration 120000 -f mp4 fragmented-aac.m4a
ffmpeg -f lavfi -i sine=sample_rate=48000:duration=2.237 -c:a opus -strict experimental -ac 2 -b:a 48k preview-opus.ogg
ffmpeg -f lavfi -i sine=sample_rate=44100:duration=1.137 -c:a flac -ac 2 preview-flac.flac
ffmpeg -f lavfi -i sine=sample_rate=44100:duration=1.137 -c:a pcm_s16le -ac 2 audio.wav
lame -V 4 audio.wav preview-mp3.mp3
ffmpeg -f lavfi -i 'sine=frequency=700:sample_rate=48000:duration=0.713' \
  -ac 2 -c:a opus -strict experimental -opus_delay 2.5 -b:a 48k opus-link.ogg
ffmpeg -f lavfi -i 'sine=frequency=500:sample_rate=48000:duration=0.75' \
  -ac 2 -c:a opus -strict experimental -b:a 48k -live 1 -f webm streaming-opus.webm
```

To recreate the tagged files, use a small PNG as `audio-cover.png`.

```sh
cp preview-flac.flac tagged-flac.flac
metaflac --set-tag='TITLE=试听 Café' --set-tag='ARTIST=Lib-Media' --set-tag='ALBUM=Audio Fixtures' \
  --set-tag='TRACKNUMBER=2/12' --set-tag='GENRE=Test' --set-tag='DATE=2026' --add-seekpoint=23040 \
  --import-picture-from='3|image/png|||audio-cover.png' tagged-flac.flac
ffmpeg -i preview-mp3.mp3 -i audio-cover.png -map 0:a -map 1:v -c copy -id3v2_version 3 \
  -metadata 'title=试听 Café' -metadata 'artist=Lib-Media' -metadata 'album=Audio Fixtures' \
  -metadata 'track=2/12' -metadata 'genre=Test' -metadata 'date=2026' \
  -metadata:s:v 'comment=Cover (front)' -disposition:v attached_pic tagged-mp3.mp3
while read -r codec frequency rate duration; do
  set -- -c:a "$codec"
  if [ "$codec" = aac ]; then set -- "$@" -profile:a aac_low -b:a 48k; fi
  ffmpeg -nostdin -f lavfi -i "sine=frequency=$frequency:sample_rate=$rate:duration=$duration" -i audio-cover.png \
    -map 0:a -map 1:v "$@" -ac 2 -c:v copy -disposition:v attached_pic \
    -metadata 'title=试听 Café' -metadata 'artist=Lib-Media' -metadata 'album=Audio Fixtures' \
    -metadata 'track=2/12' -metadata 'genre=Test' -metadata 'date=2026' -movflags +faststart "tagged-$codec.m4a"
done <<'EOF'
aac 660 48000 1.1
alac 440 96000 0.11
EOF
```

On macOS, generate the PCM fixtures and an ALAC CAF with Core Audio.

```sh
ffmpeg -f lavfi -i 'sine=sample_rate=44100:duration=0.25' -af atrim=end_sample=8821 -c:a pcm_s24le aiff-source.wav
afconvert -f AIFF -d BEI24 aiff-source.wav preview-pcm.aiff
ffmpeg -f lavfi -i 'aevalsrc=0.2*sin(2*PI*440*t)|0.15*sin(2*PI*660*t):s=48000:d=0.2' -c:a pcm_f32le caf-source.wav
afconvert -f caff -d BEF32 caf-source.wav preview-pcm.caf
afconvert -f caff -d alac tagged-alac.m4a trimmed-alac.caf
```

```sh
ffmpeg -f lavfi -i 'sine=sample_rate=44100:duration=0.25' -af atrim=end_sample=8821 -c:a pcm_f32le -f w64 preview-pcm.w64
ffmpeg -f lavfi -i 'sine=sample_rate=44100:duration=2.213' -ac 2 -c:a libvorbis preview-vorbis.ogg
```

For `trimmed-alac.caf`, set the `pakt` header's big-endian packet count, valid frames, priming, and remainder
to 3, 10540, 7, and 1741. Retain the packet lengths and audio data. The fixture keeps `desc`, `kuki`, `data`,
and `pakt` chunks.

```sh
for frames in 8821 8813; do
  ffmpeg -f lavfi -i 'sine=sample_rate=44100:duration=0.25' -af "atrim=end_sample=$frames" \
    -c:a alac -movie_timescale 1000 "alac-edit-$frames.m4a"
done
```

```sh
ffmpeg -f lavfi -i 'testsrc2=size=320x240:rate=24:duration=4' \
  -f lavfi -i 'aevalsrc=0.12*sin(2*PI*1000*t)*gte(mod(n\,48000)\,24000)*lt(mod(n\,48000)\,28000):s=48000:d=4' \
  -vf "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='between(mod(n,24),12,13)',select='not(eq(mod(n,11),5))'" \
  -vsync vfr -pix_fmt yuv420p -colorspace bt709 -color_range tv \
  -c:v libx264 -profile:v main -bf 2 -g 22 -video_track_timescale 24000 \
  -c:a aac -profile:a aac_low -ac 2 -b:a 64k preview-avc-aac.mp4
```
