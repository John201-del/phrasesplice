"""Real-media acceptance evidence inside the selected LocalCut workspace."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import numpy as np

PROJECT = Path(__file__).parents[2]
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
QA = DATA / 'tests/localai'
RUNTIME = DATA / 'local-ai'
FFMPEG = PROJECT / 'runtime/bin/ffmpeg.exe'
FFPROBE = FFMPEG.with_name('ffprobe.exe')


def probe(path):
    return json.loads(subprocess.check_output([str(FFPROBE), '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)]))


def audio(path):
    raw = subprocess.check_output([str(FFMPEG), '-v', 'error', '-i', str(path), '-f', 'f32le', '-ar', '44100', '-ac', '1', 'pipe:1'])
    return np.frombuffer(raw, '<f4').astype(np.float64)


def sisdr(reference, prediction):
    n = min(len(reference), len(prediction))
    reference, prediction = reference[:n], prediction[:n]
    reference, prediction = reference-reference.mean(), prediction-prediction.mean()
    target = np.dot(prediction, reference) / (np.dot(reference, reference)+1e-12) * reference
    return float(10*np.log10((np.dot(target, target)+1e-12)/(np.sum((prediction-target)**2)+1e-12)))


def memory(process):
    class Counters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [(key, ctypes.c_size_t) for key in ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage','QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]
    result = Counters()
    result.cb = ctypes.sizeof(result)
    function = ctypes.windll.psapi.GetProcessMemoryInfo
    function.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    if function(wintypes.HANDLE(int(process._handle)), ctypes.byref(result), result.cb):
        return result.PeakWorkingSetSize


def cpu_seconds(process):
    values = [wintypes.FILETIME() for _ in range(4)]
    function = ctypes.windll.kernel32.GetProcessTimes
    function.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)]*4
    if function(wintypes.HANDLE(int(process._handle)), *(ctypes.byref(value) for value in values)):
        return sum((value.dwHighDateTime << 32) + value.dwLowDateTime for value in values[2:])/10000000


def run(operation, source, suffix, background=False):
    output = QA / ('verify-' + operation + suffix)
    extra = []
    outputs = [output]
    if background:
        accompaniment = QA / 'verify-accompaniment.wav'
        outputs.append(accompaniment)
        extra = ['--background-output', str(accompaniment)]
    for path in outputs:
        path.unlink(missing_ok=True)
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    command = [sys.executable, str(PROJECT/'data/scripts/local_ai/process_media.py'), '--operation', operation,
               '--input', str(source), '--output', str(output), '--ffmpeg', str(FFMPEG), '--ffprobe', str(FFPROBE),
               '--runtime-root', str(RUNTIME), '--model-root', str(RUNTIME/'models'), *extra]
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak = 0
    while True:
        try:
            stdout, stderr = process.communicate(timeout=.1)
            break
        except subprocess.TimeoutExpired:
            peak = max(peak, memory(process) or 0)
    peak = max(peak, memory(process) or 0)
    result = dict(operation=operation, seconds=time.monotonic()-started, cpu_process_seconds=cpu_seconds(process), peak_process_bytes=peak, returncode=process.returncode,
                  source_sha256=original, source_unchanged=hashlib.sha256(source.read_bytes()).hexdigest()==original,
                  stdout=stdout, stderr=stderr[-8000:])
    if process.returncode == 0:
        result['outputs'] = [dict(path=str(path), bytes=path.stat().st_size, media=probe(path)) for path in outputs]
    (QA/('evidence-'+operation+'.json')).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps(dict(operation=operation, returncode=process.returncode, seconds=result['seconds'], peak_process_bytes=result['peak_process_bytes'])), flush=True)
    if process.returncode:
        print(stderr[-2000:], stdout[-2000:])
        raise RuntimeError(operation+' failed')
    return output, result


def main():
    results = []
    for operation in ('denoise_audio', 'enhance_video', 'smooth_skin', 'remove_background', 'separate_vocals'):
        is_audio = operation in ('denoise_audio', 'separate_vocals')
        source = QA / ('speech-mix.wav' if is_audio else 'portrait-test.mp4')
        output, result = run(operation, source, '.wav' if is_audio else '.mov' if operation == 'remove_background' else '.mp4', operation == 'separate_vocals')
        if is_audio:
            clean = audio(QA/'speech-clean.wav')
            result['sisdr_input_db'] = sisdr(clean, audio(source))
            result['sisdr_output_db'] = sisdr(clean, audio(output))
        if operation == 'remove_background':
            import cv2
            raw = subprocess.check_output([str(FFMPEG), '-v', 'error', '-i', str(output), '-frames:v', '1', '-pix_fmt', 'rgba', '-f', 'rawvideo', 'pipe:1'])
            frame = np.frombuffer(raw, np.uint8).reshape(480, 480, 4)
            alpha = frame[:, :, 3]
            result['alpha'] = dict(min=int(alpha.min()), max=int(alpha.max()), transparent_fraction=float(np.mean(alpha<20)), opaque_fraction=float(np.mean(alpha>235)), face_mean=float(alpha[60:140, 180:245].mean()))
            subprocess.run([str(FFMPEG), '-v', 'error', '-y', '-i', str(output), '-frames:v', '1', str(QA/'matte-preview.png')], check=True)
            rows, cols = np.indices((480, 480))
            checker = np.where(((rows//24+cols//24)%2)[..., None], 210, 150)
            composite = (frame[:, :, :3]*(alpha[..., None]/255)+checker*(1-alpha[..., None]/255)).astype(np.uint8)
            original = subprocess.check_output([str(FFMPEG), '-v', 'error', '-i', str(source), '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1'])
            original = np.frombuffer(original, np.uint8).reshape(480, 480, 3)
            cv2.imwrite(str(QA/'matte-comparison.png'), cv2.cvtColor(np.hstack((original, composite)), cv2.COLOR_RGB2BGR))
        if operation == 'smooth_skin':
            subprocess.run([str(FFMPEG), '-v', 'error', '-y', '-i', str(source), '-i', str(output), '-filter_complex', '[0:v]crop=150:180:130:20[a];[1:v]crop=150:180:130:20[b];[a][b]hstack', '-frames:v', '1', str(QA/'skin-comparison.png')], check=True)
        results.append(result)
        (QA/'model-evidence.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf8')
    print('All media transforms completed; inspect measured quality in model-evidence.json')


if __name__ == '__main__':
    main()
