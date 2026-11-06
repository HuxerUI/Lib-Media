"""Generate matched AAC-LC and H.264 sync markers for the decoded playback preview."""

import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent

with tempfile.TemporaryDirectory() as temporary:
    clip = Path(temporary) / "sync.mp4"
    subprocess.run([
        "ffmpeg", "-v", "error",
        "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=24:duration=4",
        "-f", "lavfi", "-i",
        "aevalsrc=0.12*sin(2*PI*1000*t)*gte(mod(n\\,48000)\\,24000)"
        "*lt(mod(n\\,48000)\\,28000):s=48000:d=4",
        "-vf", "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='between(mod(n,24),12,13)'",
        "-pix_fmt", "yuv420p", "-colorspace", "smpte170m", "-color_range", "tv",
        "-c:v", "libx264", "-profile:v", "main", "-bf", "2", "-g", "24", "-video_track_timescale", "24000",
        "-c:a", "aac", "-profile:a", "aac_low", "-ac", "2", "-b:a", "64k", str(clip),
    ], check=True)
    content = clip.read_bytes()

    for name, selector in (("sync-avc", "v:0"), ("sync-aac", "a:0")):
        info = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-select_streams", selector, "-show_streams", "-show_packets",
            "-show_data", "-of", "json", str(clip),
        ]))
        stream, = info["streams"]
        config = bytes.fromhex("".join(
            line.split(": ", 1)[1].split("  ", 1)[0] for line in stream["extradata"].strip().splitlines()
        ))
        if name == "sync-aac":
            config = config[:2]
        (root / f"{name}.config").write_bytes(config)
        numerator, scale = map(int, stream["time_base"].split("/"))
        data = bytearray()
        manifest = []
        for packet in info["packets"]:
            offset, size = int(packet["pos"]), int(packet["size"])
            pts, dts, duration = (int(packet[field]) * numerator for field in ("pts", "dts", "duration"))
            manifest.append(f"{len(data)} {size} {pts} {dts} {duration} {scale} {int('K' in packet['flags'])}\n")
            data.extend(content[offset:offset + size])
        (root / f"{name}.bin").write_bytes(data)
        (root / f"{name}.txt").write_text("".join(manifest))
