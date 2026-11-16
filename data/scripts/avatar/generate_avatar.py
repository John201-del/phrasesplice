"""Offline, bounded SadTalker CPU adapter. JSON stdout is the public protocol."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import re
import shutil
import subprocess

class AvatarError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__(code)

def ordinary(path):
    for part in (path, *path.parents):
        if part.is_symlink() or part.exists() and getattr(part.stat(follow_symlinks=False), 'st_file_attributes', 0) & 0x400:
            return False
    return True

def workspace_path(value):
    configured = os.environ.get('LOCALCUT_WORKSPACE_ROOT', '')
    if not configured or not Path(configured).is_absolute():
        raise AvatarError('workspace_not_configured')
    root, path = Path(configured), Path(value)
    if not path.is_absolute() or not ordinary(root) or not ordinary(path) or not path.resolve().is_relative_to(root.resolve()):
        raise AvatarError('invalid_output')
    return path.resolve()

def validate_output(value):
    path = workspace_path(value)
    if path.suffix.lower() != '.mp4' or not path.parent.is_dir():
        raise AvatarError('invalid_output')
    if path.exists() and (not path.is_file() or path.stat().st_size):
        raise AvatarError('output_exists')
    return path

def emit(value):
    print(json.dumps(value, ensure_ascii=True), flush=True)

def publish(source, output):
    """Keep validation and write under one exclusive Windows file handle."""
    import ctypes
    import msvcrt
    from ctypes import wintypes
    create = ctypes.WinDLL('kernel32', use_last_error=True).CreateFileW
    create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    create.restype = wintypes.HANDLE
    handle = create(str(output), 0xC0000000, 0, None, 4, 0x00200080, None)
    if handle == ctypes.c_void_p(-1).value:
        raise AvatarError('output_unavailable')
    descriptor = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
    with os.fdopen(descriptor, 'r+b') as destination:
        if os.fstat(destination.fileno()).st_size or not ordinary(output):
            raise AvatarError('output_exists')
        with source.open('rb') as stream:
            shutil.copyfileobj(stream, destination)

def input_file(value):
    path = Path(value)
    if not path.is_absolute() or not re.fullmatch(r'[A-Za-z]:', path.drive) or any(':' in part for part in path.parts[1:]) or str(path).startswith(('\\\\', '//')) or not ordinary(path) or not path.is_file():
        raise AvatarError('invalid_input')
    return path.resolve()

def probe(executable, path):
    result = subprocess.run([str(executable), '-v', 'error', '-protocol_whitelist', 'file,pipe', '-show_streams', '-show_format', '-of', 'json', str(path)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, encoding='utf-8', errors='replace', timeout=45)
    if result.returncode:
        raise AvatarError('invalid_audio')
    try:
        return json.loads(result.stdout)
    except ValueError:
        raise AvatarError('invalid_audio')

def main():
    parser = argparse.ArgumentParser()
    for name in ('image', 'audio', 'output', 'runtime-root', 'ffmpeg', 'ffprobe'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    output = validate_output(args.output)
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', output.stem):
        raise AvatarError('invalid_output')
    image, audio = input_file(args.image), input_file(args.audio)
    root = workspace_path(args.runtime_root)
    for directory in ('tmp', 'cache/matplotlib', 'cache/tmp', 'jobs'):
        workspace_path(root / directory)
    source = root / 'source' / 'SadTalker'
    if not ordinary(source / 'inference.py') or not (source / 'inference.py').is_file():
        raise AvatarError('runtime_missing')
    models = json.loads(Path(__file__).with_name('model_artifacts.json').read_text(encoding='utf-8'))
    for artifact in models:
        model = root / 'models' / artifact['name']
        if not ordinary(model) or not model.is_file() or model.stat().st_size != artifact['bytes']:
            raise AvatarError('models_missing')
        with model.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != artifact['sha256']:
                raise AvatarError('models_invalid')
    ffmpeg, ffprobe = input_file(args.ffmpeg), input_file(args.ffprobe)
    metadata = probe(ffprobe, audio)
    streams = metadata.get('streams', [])
    try:
        duration = float(metadata['format']['duration'])
    except (KeyError, ValueError, TypeError):
        raise AvatarError('invalid_audio')
    if not any(s.get('codec_type') == 'audio' for s in streams) or not .12 <= duration <= 30:
        raise AvatarError('audio_too_long' if duration > 30 else 'invalid_audio')
    jobs = root / 'jobs'
    jobs.mkdir(exist_ok=True)
    if not ordinary(jobs):
        raise AvatarError('runtime_missing')
    job = jobs / output.stem
    try:
        job.mkdir()
    except FileExistsError:
        raise AvatarError('job_exists') from None
    try:
        emit({'progress': 1})
        from PIL import Image
        with Image.open(image) as picture:
            picture.convert('RGB').save(job / 'portrait.png')
        convert = subprocess.run([str(ffmpeg), '-nostdin', '-v', 'error', '-y', '-protocol_whitelist', 'file,pipe', '-i', str(audio), '-vn', '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', str(job / 'speech.wav')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
        if convert.returncode:
            raise AvatarError('invalid_audio')
        environment = os.environ.copy()
        environment.update({'OMP_NUM_THREADS': '4', 'MKL_NUM_THREADS': '4', 'OPENBLAS_NUM_THREADS': '4', 'NUMBA_NUM_THREADS': '4', 'PYTHONNOUSERSITE': '1', 'PYTHONUTF8': '1', 'PYTHONIOENCODING': 'utf-8', 'TEMP': str(root / 'tmp'), 'TMP': str(root / 'tmp'), 'TORCH_HOME': str(root / 'cache'), 'NUMBA_CACHE_DIR': str(root / 'cache'), 'MPLCONFIGDIR': str(root / 'cache/matplotlib'), 'MPLBACKEND': 'Agg', 'JOBLIB_TEMP_FOLDER': str(root / 'cache/tmp'), 'XDG_CACHE_HOME': str(root / 'cache'), 'AVATAR_FFMPEG': str(ffmpeg), 'AVATAR_FFPROBE': str(ffprobe), 'AVATAR_MODELS': str(root / 'models'), 'IMAGEIO_FFMPEG_EXE': str(ffmpeg), 'PATH': str(ffmpeg.parent) + os.pathsep + environment.get('PATH', '')})
        emit({'progress': 5})
        command = [sys.executable, str(Path(__file__).with_name('run_inference.py')), str(source), str(root / 'models'), str(job)]
        process = subprocess.Popen(command, cwd=source, env=environment, stdout=sys.stderr, stderr=subprocess.PIPE, encoding='utf-8', errors='replace')
        progress = 5
        with process.stderr:
            for line in process.stderr:
                print(line, end='', file=sys.stderr)
                rendered = re.match(r'Face Renderer::\s+(\d+)%', line)
                if rendered:
                    next_progress = 5 + min(int(rendered[1]), 100) * 94 // 100
                    if next_progress > progress:
                        progress = next_progress
                        emit({'progress': progress})
        status = process.wait()
        if status:
            raise AvatarError('no_face' if status == 20 else 'generation_failed')
        results = list((job / 'results').glob('*.mp4'))
        if len(results) != 1:
            raise AvatarError('no_face')
        final = probe(ffprobe, results[0])
        video = next((s for s in final.get('streams', []) if s.get('codec_type') == 'video'), {})
        if video.get('width') != 256 or video.get('height') != 256 or video.get('r_frame_rate') != '25/1' or not any(s.get('codec_type') == 'audio' for s in final.get('streams', [])):
            raise AvatarError('verification_failed')
        if abs(float(final['format']['duration']) - duration) > .25:
            raise AvatarError('verification_failed')
        # The caller reserves a zero-byte output. Recheck immediately before publishing.
        validate_output(str(output))
        publish(results[0], output)
        emit({'progress': 100})
        emit({'ok': True, 'path': str(output)})
    finally:
        shutil.rmtree(job)

if __name__ == '__main__':
    try:
        main()
    except AvatarError as error:
        emit({'ok': False, 'code': error.code})
        sys.exit(1)
    except Exception as error:
        print(type(error).__name__ + ': ' + str(error), file=sys.stderr)
        emit({'ok': False, 'code': 'generation_failed'})
        sys.exit(1)
