"""Protocol fixture: real ffmpeg output without model weights."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser()
for name in ('image', 'audio', 'output', 'runtime-root', 'ffmpeg', 'ffprobe'):
    p.add_argument('--' + name, required=True)
a = p.parse_args()
mode = os.environ.get('AVATAR_TEST_MODE', 'valid')
if mode == 'flood':
    print('x' * 1100000, flush=True)
    raise SystemExit(0)
if mode == 'cancel':
    job = Path(a.runtime_root) / 'jobs' / Path(a.output).stem
    job.mkdir(parents=True)
    child = subprocess.Popen([a.ffmpeg, '-hide_banner', '-loglevel', 'error', '-re', '-f', 'lavfi', '-i', 'anullsrc', '-f', 'null', '-'])
    (Path(a.runtime_root) / 'child.pid').write_text(str(child.pid))
    print(json.dumps({'progress': 1}), flush=True)
    time.sleep(60)
if mode == 'mutate':
    with open(a.audio, 'ab') as f:
        f.write(b'changed')
if mode == 'output_conflict':
    Path(a.output).write_bytes(b'external output')
    print(json.dumps({'ok': False, 'code': 'output_exists'}), flush=True)
    raise SystemExit(1)
if mode == 'job_conflict':
    job = Path(a.runtime_root) / 'jobs' / Path(a.output).stem
    job.mkdir(parents=True)
    (job / 'keep.txt').write_text('external job')
    print(json.dumps({'ok': False, 'code': 'job_exists'}), flush=True)
    raise SystemExit(1)
if mode != 'bad':
    size = '128:128' if mode == 'geometry' else '256:256'
    cmd = [a.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-loop', '1', '-i', a.image, '-i', a.audio,
           '-vf', 'scale=' + size, '-c:v', 'libx264', '-threads', '1', '-pix_fmt', 'yuv420p', '-r', '25']
    cmd += ['-an', '-t', '1'] if mode == 'silent' else ['-c:a', 'aac', '-t', '1']
    if mode == 'duration':
        cmd += ['-t', '0.2']
    if mode == 'shortaudio':
        cmd += ['-af', 'atrim=duration=0.2']
    subprocess.run(cmd + [a.output], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(json.dumps({'ok': True, 'path': str(Path(a.output))}), flush=True)
