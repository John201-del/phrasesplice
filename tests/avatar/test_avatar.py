import importlib.util
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
import json
import ast
import os
from types import SimpleNamespace

SCRIPT = Path(__file__).resolve().parents[2] / 'data/scripts/avatar/generate_avatar.py'
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
WORK = DATA / 'tests/avatar'

class Contract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        WORK.mkdir(parents=True, exist_ok=True)

    def test_reparse_output_is_rejected(self):
        import _winapi
        spec = importlib.util.spec_from_file_location('avatar', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(dir=WORK) as directory:
            base = Path(directory)
            target, link = base / 'real', base / 'link'
            target.mkdir()
            _winapi.CreateJunction(str(target), str(link))
            with self.assertRaises(module.AvatarError) as error:
                module.validate_output(str(link / 'result.mp4'))
            self.assertEqual(error.exception.code, 'invalid_output')
            link.rmdir()

    def test_no_face_is_a_bounded_error(self):
        source = DATA / 'local-avatar/source/SadTalker/src/utils/croper.py'
        if not source.is_file():
            self.skipTest('Optional SadTalker runtime is not installed')
        tree = ast.parse(source.read_text(encoding='utf-8'))
        klass = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'Preprocesser')
        method = next(node for node in klass.body if isinstance(node, ast.FunctionDef) and node.name == 'crop')
        namespace = {}
        exec(compile(ast.Module(body=[method], type_ignores=[]), str(source), 'exec'), namespace)
        try:
            namespace['crop'](SimpleNamespace(get_landmark=lambda _: None), [object()])
        except Exception as error:
            self.assertIsInstance(error, ValueError)
            self.assertEqual(str(error), 'AVATAR_NO_FACE')
        else:
            self.fail('missing face must fail')

    def test_publication_preserves_nonempty_output(self):
        spec = importlib.util.spec_from_file_location('avatar', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, 'publish'), 'publication needs an exclusive output handle')
        with tempfile.TemporaryDirectory(dir=WORK) as directory:
            base = Path(directory)
            source, output = base / 'source.mp4', base / 'result.mp4'
            source.write_bytes(b'result')
            output.write_bytes(b'existing')
            with self.assertRaises(module.AvatarError):
                module.publish(source, output)
            self.assertEqual(output.read_bytes(), b'existing')
            output.write_bytes(b'')
            module.publish(source, output)
            self.assertEqual(output.read_bytes(), b'result')

    def test_missing_models_and_input_intact(self):
        with tempfile.TemporaryDirectory(dir=WORK) as directory:
            base = Path(directory)
            (base / 'source/SadTalker').mkdir(parents=True)
            (base / 'source/SadTalker/inference.py').touch()
            image = base / 'i.png'
            audio = base / 'a.wav'
            image.write_bytes(b'image')
            audio.write_bytes(b'audio')
            process = subprocess.run([sys.executable, str(SCRIPT), '--image', str(image), '--audio', str(audio), '--output', str(base / 'test.mp4'), '--runtime-root', str(base), '--ffmpeg', str(image), '--ffprobe', str(image)], capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(json.loads(process.stdout), {'ok': False, 'code': 'models_missing'})
            self.assertEqual(image.read_bytes(), b'image')
            self.assertEqual(audio.read_bytes(), b'audio')
            self.assertFalse((base / 'jobs').exists())

    def test_cli_missing_runtime_is_json_failure(self):
        with tempfile.TemporaryDirectory(dir=WORK) as directory:
            base = Path(directory)
            image = base / 'i.png'
            audio = base / 'a.wav'
            image.write_bytes(b'image')
            audio.write_bytes(b'audio')
            process = subprocess.run([sys.executable, str(SCRIPT), '--image', str(image), '--audio', str(audio), '--output', str(base / 'test.mp4'), '--runtime-root', str(base), '--ffmpeg', str(image), '--ffprobe', str(image)], capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(json.loads(process.stdout), {'ok': False, 'code': 'runtime_missing'})

    def test_output_contract(self):
        self.assertTrue(SCRIPT.is_file(), 'avatar runner must exist')
        spec = importlib.util.spec_from_file_location('avatar', SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(dir=WORK) as directory:
            output = Path(directory) / 'reserved.mp4'
            output.touch()
            self.assertEqual(module.validate_output(str(output)), output.resolve())
            output.write_bytes(b'existing')
            with self.assertRaises(module.AvatarError) as error:
                module.validate_output(str(output))
            self.assertEqual(error.exception.code, 'output_exists')
            with self.assertRaises(module.AvatarError) as error:
                module.validate_output('C:/avatar.mp4')
            self.assertEqual(error.exception.code, 'invalid_output')

if __name__ == '__main__':
    unittest.main()
