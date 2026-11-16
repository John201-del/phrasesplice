"""Storage contracts without downloading models or running inference."""
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest
import wave
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[2]


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, PROJECT / relative)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class WorkspaceContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ['LOCALCUT_WORKSPACE_ROOT'])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / 'data'
        self.data.mkdir()
        self.environment = patch.dict(os.environ, LOCALCUT_WORKSPACE_ROOT=str(self.data))
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.media = module('local_media', 'data/scripts/local_ai/process_media.py')
        self.avatar = module('avatar', 'data/scripts/avatar/generate_avatar.py')

    def validate(self, output):
        return (lambda: self.media.validate_output(self.root / 'source.wav', output),
                lambda: self.avatar.validate_output(str(output)))

    def test_reserved_outputs_in_selected_root_are_accepted(self):
        output = self.data / 'reserved.mp4'
        output.touch()
        for validate in self.validate(output):
            validate()

    def test_missing_environment_is_explicit(self):
        with patch.dict(os.environ):
            os.environ.pop('LOCALCUT_WORKSPACE_ROOT')
            for validate in self.validate(self.data / 'result.mp4'):
                with self.assertRaises(Exception) as error:
                    validate()
                self.assertIn('workspace', str(error.exception).lower())

    def test_outside_and_traversal_are_rejected(self):
        for output in (self.root / 'outside.mp4', self.data / '..' / 'outside.mp4'):
            for validate in self.validate(output):
                with self.assertRaises(Exception):
                    validate()

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_junction_is_rejected_even_when_target_is_inside(self):
        import _winapi
        target, link = self.data / 'real', self.data / 'link'
        target.mkdir()
        _winapi.CreateJunction(str(target), str(link))
        try:
            for validate in self.validate(link / 'result.mp4'):
                with self.assertRaises(Exception):
                    validate()
        finally:
            link.rmdir()

    def test_existing_output_is_preserved(self):
        output = self.data / 'result.mp4'
        output.write_bytes(b'keep')
        for validate in self.validate(output):
            with self.assertRaises(Exception):
                validate()
        self.assertEqual(output.read_bytes(), b'keep')

    def test_photo_cli_requires_workspace_before_processing(self):
        output = self.data / 'reserved.png'
        output.touch()
        command = [sys.executable, '-I', '-B', str(PROJECT / 'data/scripts/local_ai/retouch_photo.py'),
                   '--input', str(self.data / 'missing.jpg'), '--output', str(output),
                   '--model-root', str(self.data / 'models'), '--options', '{"skinStrength":30}']
        with patch.dict(os.environ):
            os.environ.pop('LOCALCUT_WORKSPACE_ROOT')
            result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'workspace_not_configured')
        self.assertEqual(output.read_bytes(), b'')

    def test_face_cli_reports_missing_workspace_before_loading_models(self):
        command = [sys.executable, '-I', '-B', str(PROJECT / 'data/scripts/local_ai/process_media.py'),
            '--operation', 'animate_face', '--input', str(self.data / 'input.mp4'), '--output', str(self.data / 'output.mp4'),
            '--ffmpeg', str(self.data / 'ffmpeg.exe'), '--ffprobe', str(self.data / 'ffprobe.exe'),
            '--model-root', str(self.data / 'models'), '--runtime-root', str(self.data / 'local-avatar')]
        with patch.dict(os.environ):
            os.environ.pop('LOCALCUT_WORKSPACE_ROOT')
            result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'workspace_not_configured')

    def test_photo_cli_exports_in_selected_root_without_changing_source(self):
        import cv2
        import numpy as np
        source, output = self.data / 'source.png', self.data / 'reserved.png'
        image = np.full((48, 48, 3), 180, np.uint8)
        image[23:26, 23:26] = 0
        source.write_bytes(cv2.imencode('.png', image)[1].tobytes())
        before = source.read_bytes()
        output.touch()
        result = subprocess.run([sys.executable, '-I', '-B', str(PROJECT / 'data/scripts/local_ai/retouch_photo.py'),
            '--input', str(source), '--output', str(output), '--model-root', str(self.data / 'models'),
            '--options', '{"blemishes":[{"xPercent":50,"yPercent":50,"radiusPercent":5}]}'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)['path'], str(output))
        self.assertEqual(cv2.imdecode(np.fromfile(output, np.uint8), cv2.IMREAD_COLOR).shape, image.shape)
        self.assertEqual(source.read_bytes(), before)

    def test_audio_processing_uses_selected_root(self):
        ffmpeg = shutil.which('ffmpeg.exe')
        if not ffmpeg or not Path(ffmpeg).with_name('ffprobe.exe').is_file():
            self.skipTest('FFmpeg and ffprobe must be on PATH for real-media verification')
        source, output = self.data / 'source.wav', self.data / 'result.wav'
        with wave.open(str(source), 'wb') as audio:
            audio.setparams((1, 2, 16000, 0, 'NONE', 'none'))
            audio.writeframes(bytes(32000))
        before = source.read_bytes()
        result = subprocess.run([sys.executable, '-I', '-B', str(PROJECT / 'data/scripts/local_ai/process_media.py'),
            '--operation', 'denoise_audio', '--input', str(source), '--output', str(output), '--ffmpeg', ffmpeg,
            '--ffprobe', str(Path(ffmpeg).with_name('ffprobe.exe')), '--model-root', str(self.data / 'models'),
            '--runtime-root', str(self.data / 'local-ai')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(json.loads(result.stdout.splitlines()[-1])['ok'])
        with wave.open(str(output), 'rb') as audio:
            self.assertEqual(audio.getnframes(), 16000)
        self.assertEqual(source.read_bytes(), before)

    def test_cached_download_is_reusable_and_confined(self):
        downloader = module('avatar_download', 'data/scripts/avatar/download_file.py')
        cached = self.data / 'artifact.bin'
        cached.write_bytes(b'cached')
        digest = hashlib.sha256(b'cached').hexdigest()
        for _ in range(2):
            self.assertEqual(downloader.download('https://invalid.invalid/artifact', cached, 6, digest), digest)
        with self.assertRaises(Exception):
            downloader.download('https://invalid.invalid/artifact', self.root / 'outside.bin', 6, digest)
        self.assertFalse((self.root / 'outside.bin.parts').exists())

    def test_installers_reject_outside_root_without_creating_it(self):
        for script, flag in (('setup-local-ai.ps1', '-Root'), ('setup-local-avatar.ps1', '-RuntimeRoot')):
            with self.subTest(script=script):
                outside = self.root / script
                powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
                environment = dict(os.environ, PATH='')  # A regression must never start a downloader.
                result = subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass',
                    '-File', str(PROJECT / 'scripts' / script), flag, str(outside)], env=environment,
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(outside.exists(), result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
