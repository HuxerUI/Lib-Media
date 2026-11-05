"""Regenerate project-owned synthetic access units; FFmpeg is a fixture tool only."""
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as temporary:
    for name, source, options in (
        ('aac', 'sine=frequency=440:sample_rate=48000:duration=0.25', ['-ac', '2', '-c:a', 'aac', '-b:a', '96k', '-profile:a', 'aac_low']),
        ('avc', 'testsrc=size=64x48:rate=24:duration=1', ['-pix_fmt', 'yuv420p', '-c:v', 'libx264', '-profile:v', 'main', '-bf', '2', '-g', '24', '-video_track_timescale', '24000']),
    ):
        path = Path(temporary) / (name + '.mp4')
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', source, *options, str(path)], check=True)
        metadata = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-show_packets', '-show_data', '-of', 'json', str(path)]))
        stream = metadata['streams'][0]
        def unhex(dump):
            return bytes.fromhex(''.join(line.split(':', 1)[1].split('  ')[0].replace(' ', '') for line in dump.strip().splitlines()))
        config = unhex(stream['extradata'])
        if name == 'aac':
            # The encoder adds an optional SBR absence marker; the fixture explicitly declares AAC-LC.
            config = config[:2]
        (root / (name + '.config')).write_bytes(config)
        content = path.read_bytes()
        manifest = []
        numerator, scale = map(int, stream['time_base'].split('/'))
        for i, packet in enumerate(metadata['packets']):
            filename = f'{name}-{i:03d}.bin'
            offset, size = int(packet['pos']), int(packet['size'])
            (root / filename).write_bytes(content[offset:offset + size])
            manifest.append(f"{filename} {int(packet['pts']) * numerator} {int(packet['dts']) * numerator} {int(packet['duration']) * numerator} {scale} {int('K' in packet['flags'])}\n")
        (root / (name + '.txt')).write_text(''.join(manifest))
