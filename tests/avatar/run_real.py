"""Two-second local acceptance run; requires psutil and prepared media fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import psutil

PROJECT = Path(__file__).resolve().parents[2]
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
QA = DATA / 'tests/avatar'
RUNTIME = DATA / 'local-avatar'
PYTHON = RUNTIME / 'python/python.exe'
FFMPEG = PROJECT / 'runtime/bin/ffmpeg.exe'
FFPROBE = FFMPEG.with_name('ffprobe.exe')
SCRIPT = PROJECT / 'data/scripts/avatar/generate_avatar.py'

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def run(label, image, audio):
    output = QA / (label + '.mp4')
    if output.exists():
        raise RuntimeError('Acceptance outputs must not be overwritten')
    output.touch()  # Exercise the same empty reservation used by the Qt client.
    before = {'image': sha(image), 'audio': sha(audio)}
    command = [str(PYTHON), str(SCRIPT), '--image', str(image), '--audio', str(audio), '--output', str(output), '--runtime-root', str(RUNTIME), '--ffmpeg', str(FFMPEG), '--ffprobe', str(FFPROBE)]
    started = time.monotonic()
    peak = 0
    with (QA / (label + '.stdout.jsonl')).open('w', encoding='utf-8') as stdout, (QA / (label + '.stderr.log')).open('w', encoding='utf-8') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        root = psutil.Process(process.pid)
        while process.poll() is None:
            try:
                total = 0
                for child in [root, *root.children(recursive=True)]:
                    try:
                        total += child.memory_info().rss
                    except psutil.NoSuchProcess:
                        pass
                peak = max(peak, total)
            except psutil.NoSuchProcess:
                pass
            time.sleep(.25)
    elapsed = time.monotonic() - started
    after = {'image': sha(image), 'audio': sha(audio)}
    result = {'label': label, 'exit_code': process.returncode, 'elapsed_seconds': elapsed, 'sampled_peak_process_tree_rss_bytes': peak, 'sampling_interval_seconds': .25, 'source_sha256_before': before, 'source_sha256_after': after, 'sources_unchanged': before == after, 'output': str(output), 'job_cleaned': not (RUNTIME / 'jobs' / label).exists()}
    if process.returncode == 0:
        probe = subprocess.run([str(FFPROBE), '-v', 'error', '-count_frames', '-show_streams', '-show_format', '-of', 'json', str(output)], capture_output=True, text=True, check=True)
        result['probe'] = json.loads(probe.stdout)
        video = next(stream for stream in result['probe']['streams'] if stream['codec_type'] == 'video')
        soundtrack = next(stream for stream in result['probe']['streams'] if stream['codec_type'] == 'audio')
        assert (video['codec_name'], video['width'], video['height'], video['r_frame_rate'], int(video['nb_read_frames'])) == ('h264', 256, 256, '25/1', 50)
        assert abs(float(soundtrack['duration']) - 2) < .1
        result['output_sha256'] = sha(output)
        subprocess.run([str(FFMPEG), '-nostdin', '-v', 'error', '-i', str(output), '-vf', 'select=eq(n\\,0)+eq(n\\,10)+eq(n\\,20)+eq(n\\,30)+eq(n\\,40)', '-vsync', '0', str(QA / (label + '-%02d.png'))], check=True)
    (QA / (label + '-report.json')).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'probe'}), flush=True)
    if process.returncode or before != after or not result['job_cleaned']:
        raise RuntimeError('Real generation acceptance failed')

if __name__ == '__main__':
    silence = QA / 'silence2.wav'
    if not silence.exists():
        subprocess.run([str(FFMPEG), '-nostdin', '-n', '-v', 'error', '-f', 'lavfi', '-i', 'anullsrc=r=16000:cl=mono', '-t', '2', '-c:a', 'pcm_s16le', str(silence)], check=True)
    run('speech-result', QA / 'portrait.png', QA / 'speech2.wav')
    run('silence-result', QA / 'portrait.png', silence)
