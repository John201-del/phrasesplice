"""Real executable/media failure contracts; requires the installed avatar runtime."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest

PROJECT = Path(__file__).resolve().parents[2]
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
QA = DATA / 'tests/avatar'
RUNTIME = DATA / 'local-avatar'
PYTHON = RUNTIME / 'python/python.exe'
FFMPEG = PROJECT / 'runtime/bin/ffmpeg.exe'
FFPROBE = FFMPEG.with_name('ffprobe.exe')
SCRIPT = PROJECT / 'data/scripts/avatar/generate_avatar.py'

@unittest.skipUnless(PYTHON.is_file() and FFMPEG.is_file() and (QA / 'portrait.png').is_file() and (QA / 'speech2.wav').is_file(),
                     'Requires optional avatar runtime and portrait/speech fixtures in the selected workspace')
class RuntimeFailures(unittest.TestCase):
    def call(self, image, audio, output, expected):
        inputs = [Path(image), Path(audio)]
        before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs]
        result = subprocess.run([str(PYTHON), str(SCRIPT), '--image', str(image), '--audio', str(audio), '--output', str(output), '--runtime-root', str(RUNTIME), '--ffmpeg', str(FFMPEG), '--ffprobe', str(FFPROBE)], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=240)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        lines = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(lines[-1], {'ok': False, 'code': expected})
        self.assertEqual(before, [hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs])
        self.assertFalse((RUNTIME / 'jobs' / Path(output).stem).exists())
        (QA / (Path(output).stem + '-failure.json')).write_text(json.dumps({'exit': result.returncode, 'protocol': lines, 'input_sha256': before, 'job_cleaned': True}, indent=2), encoding='utf-8')

    def test_bad_audio(self):
        audio = QA / 'bad-audio.wav'
        audio.write_bytes(b'not wave audio')
        self.call(QA / 'portrait.png', audio, QA / 'bad-audio-output.mp4', 'invalid_audio')

    def test_outside_output(self):
        self.call(QA / 'portrait.png', QA / 'speech2.wav', Path('C:/avatar-should-never-exist.mp4'), 'invalid_output')

    def test_nonempty_output(self):
        output = QA / 'nonempty-output.mp4'
        output.write_bytes(b'preserve this existing content')
        self.call(QA / 'portrait.png', QA / 'speech2.wav', output, 'output_exists')
        self.assertEqual(output.read_bytes(), b'preserve this existing content')

    def test_bad_image_cleans_created_job(self):
        image = QA / 'bad-image.png'
        image.write_bytes(b'not an image')
        self.call(image, QA / 'speech2.wav', QA / 'bad-image-output.mp4', 'generation_failed')

    def test_no_face(self):
        from PIL import Image
        image = QA / 'blank.png'
        Image.new('RGB', (256, 256), 'white').save(image)
        self.call(image, QA / 'speech2.wav', QA / 'no-face-output.mp4', 'no_face')

    def test_audio_over_30_seconds(self):
        audio = QA / 'silence31.wav'
        subprocess.run([str(FFMPEG), '-nostdin', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'anullsrc=r=16000:cl=mono', '-t', '31', '-c:a', 'pcm_s16le', str(audio)], check=True)
        self.call(QA / 'portrait.png', audio, QA / 'long-audio-output.mp4', 'audio_too_long')

if __name__ == '__main__':
    unittest.main()
