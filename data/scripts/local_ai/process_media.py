"""Offline local media transforms. Models are installed separately, never fetched here."""
import argparse
from contextlib import redirect_stdout
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import wave

FACE_OPERATIONS = ('cover_face', 'animate_face')
OPERATIONS = ('denoise_audio', 'separate_vocals', 'remove_background', 'enhance_video', 'smooth_skin', *FACE_OPERATIONS)
CHILDREN = []


class WorkspaceNotConfigured(ValueError):
    pass


def emit(**value):
    if 'progress' in value:
        value['progress'] = int(value['progress'])
    print(json.dumps(value, ensure_ascii=True), flush=True)


def workspace_path(value):
    configured = os.environ.get('LOCALCUT_WORKSPACE_ROOT', '')
    if not configured or not Path(configured).is_absolute():
        raise WorkspaceNotConfigured('LOCALCUT_WORKSPACE_ROOT must name an absolute data directory')
    root, path = Path(configured), Path(value)
    if not path.is_absolute():
        raise ValueError('Workspace paths must be absolute')
    for item in (root, *root.parents, path, *path.parents):
        if item.is_symlink() or item.exists() and getattr(item.lstat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Workspace paths must not use links or reparse points')
    path = path.resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('Path must be inside LOCALCUT_WORKSPACE_ROOT')
    return path


def validate_output(source, output):
    source, output = Path(source).resolve(), workspace_path(output)
    if source == output:
        raise ValueError('Output must differ from input')
    if output.exists() and (not output.is_file() or output.stat().st_size):
        raise ValueError('Refusing to overwrite existing nonempty output')


def spawn(command, **kwargs):
    process = subprocess.Popen(command, **kwargs)
    CHILDREN.append(process)
    return process


def drain_errors(process):
    tail = bytearray()
    def drain():
        while block := process.stderr.read(4096):
            tail.extend(block)
            del tail[:-4096]
    thread = threading.Thread(target=drain, daemon=True)
    thread.start()
    return tail, thread


def checked(command):
    process = spawn(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output, error = process.communicate()
    if process.returncode:
        raise RuntimeError(error.decode('utf8', errors='replace')[-1800:])
    return output


def probe(args, path):
    protocol = ['-protocol_whitelist', 'file,pipe'] if args.operation in FACE_OPERATIONS else []
    return json.loads(checked([args.ffprobe, '-v', 'error', *protocol, '-show_streams', '-show_format', '-of', 'json', str(path)]))


def ffbase(args):
    return [args.ffmpeg, '-hide_banner', '-loglevel', 'error', '-xerror', '-nostdin', '-y', '-threads', '2', '-filter_threads', '2']


def video_geometry(video):
    sar = video.get('sample_aspect_ratio')
    if sar not in (None, '', 'N/A', '0:1'):
        numerator, denominator = map(int, sar.split(':'))
        if numerator <= 0 or denominator <= 0 or numerator != denominator:
            raise ValueError('Non-square sample aspect ratio is unsupported')

    def right_angle(value):
        angle = float(value)
        if not math.isfinite(angle) or abs(angle / 90 - round(angle / 90)) > 1e-6:
            raise ValueError('Only orthogonal 90-degree video rotation is supported')
        return int(round(angle / 90) * 90) % 360

    matrices = [data for data in video.get('side_data_list', []) if data.get('side_data_type') == 'Display Matrix']
    if matrices:
        if len(matrices) != 1:
            raise ValueError('Multiple display matrices are unsupported')
        data = matrices[0]
        values = tuple(int(value) for line in data.get('displaymatrix', '').splitlines() if ':' in line for value in line.split(':', 1)[1].split())
        canonical = {
            (65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824): 0,
            (0, -65536, 0, 65536, 0, 0, 0, 0, 1073741824): 90,
            (-65536, 0, 0, 0, -65536, 0, 0, 0, 1073741824): 180,
            (0, 65536, 0, -65536, 0, 0, 0, 0, 1073741824): 270,
        }
        if values not in canonical:
            raise ValueError('Non-canonical display matrix (mirror/scale/translation/perspective) is unsupported')
        rotation = canonical[values]
        if 'rotation' in data and right_angle(data['rotation']) != rotation:
            raise ValueError('Display matrix and rotation disagree')
    else:
        rotation = right_angle(video.get('tags', {}).get('rotate', 0))
    width, height = video['width'], video['height']
    return (height, width, rotation) if rotation in (90, 270) else (width, height, rotation)


def traditional(args, duration, metadata):
    command = ffbase(args)
    if args.operation == 'enhance_video':
        video = next(s for s in metadata['streams'] if s['codec_type'] == 'video')
        command += ['-display_rotation', str(video_geometry(video)[2])]
    command += ['-i', args.input]
    if args.operation == 'denoise_audio':
        audio = next(s for s in metadata['streams'] if s['codec_type'] == 'audio')
        # afftdn buffers two sample advances (FFmpeg af_afftdn.c). Flush the
        # tail before trimming this latency so the source clock stays intact.
        latency = 2 * (int(audio['sample_rate']) // 80)
        audio_filter = f'apad=pad_len={latency},afftdn=nf=-25,atrim=start_sample={latency},asetpts=PTS-STARTPTS'
        command += ['-map', '0:a:0', '-af', audio_filter, '-c:a', 'pcm_s16le', '-f', 'wav']
    else:
        command += ['-map', '0:v:0', '-map', '0:a?', '-metadata:s:v:0', 'rotate=0', '-vf', 'hqdn3d=1.5:1.5:3:3,unsharp=5:5:0.4:5:5:0', '-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-f', 'mp4']
    process = spawn(command + ['-threads', '2', '-progress', 'pipe:1', args.output], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    errors, error_thread = drain_errors(process)
    for line in process.stdout:
        if line.startswith(b'out_time_us=') and duration:
            try:
                emit(progress=min(99, int(line.split(b'=')[1]) / 10000 / duration))
            except ValueError:
                pass
    code = process.wait()
    error_thread.join()
    if code:
        raise RuntimeError(errors.decode('utf8', errors='replace')[-1800:])
    return 'Traditional FFmpeg audio denoise' if args.operation == 'denoise_audio' else 'Traditional FFmpeg gentle video denoise and sharpening'


def read_exact(pipe, size):
    result = bytearray()
    while len(result) < size:
        block = pipe.read(size - len(result))
        if not block:
            break
        result.extend(block)
    return bytes(result)


def video_frames(args, metadata, duration, photo_module=None):
    import numpy as np
    import cv2
    cv2.setNumThreads(2)
    video = next((s for s in metadata['streams'] if s['codec_type'] == 'video'), None)
    if not video:
        raise ValueError('Input has no video stream')
    width, height, rotation = video_geometry(video)
    rate = video.get('avg_frame_rate', '0/0')
    numerator, denominator = map(int, rate.split('/'))
    if not numerator or not denominator:
        raise ValueError('Cannot determine video frame rate')
    fps = numerator / denominator
    remove = args.operation == 'remove_background'
    cover = args.operation in FACE_OPERATIONS
    if remove:
        import onnxruntime as ort
        ort.disable_telemetry_events()
        model = workspace_path(Path(args.model_root) / 'rvm_mobilenetv3_fp32.onnx')
        if not model.is_file():
            raise ValueError('RVM model missing: run setup-local-ai.ps1 first')
        options = ort.SessionOptions()
        options.intra_op_num_threads = 2
        options.inter_op_num_threads = 1
        session = ort.InferenceSession(str(model), options, providers=['CPUExecutionProvider'])
        states = [np.zeros((1, 1, 1, 1), np.float32) for _ in range(4)]
        ratio = np.array([min(512 / max(width, height), 1)], np.float32)
    elif cover:
        if args.operation == 'animate_face':
            if duration > 30 or fps > 60 or width * height > 3840 * 2160:
                raise photo_module.PhotoError('invalid_animated_video')
            face_cover = photo_module.AnimatedFace(args.model_root)
        else:
            if duration > 120 or not args.sticker:
                raise photo_module.PhotoError('invalid_cover_video')
            face_cover = photo_module.FaceCover(args.sticker, video=True)
    else:
        cascade = cv2.CascadeClassifier(str(Path(cv2.data.haarcascades) / 'haarcascade_frontalface_default.xml'))
        if cascade.empty():
            raise ValueError('Face detector missing')
    protocol = ['-protocol_whitelist', 'file,pipe'] if cover else []
    reader = spawn(ffbase(args) + protocol + ['-display_rotation', str(rotation), '-i', args.input, '-map', '0:v:0', '-vf', 'fps=' + rate, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-threads', '2', 'pipe:1'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    read_errors, read_error_thread = drain_errors(reader)
    command = ffbase(args) + ['-f', 'rawvideo', '-pix_fmt', 'rgba' if remove else 'rgb24', '-s', f'{width}x{height}', '-r', rate, '-i', 'pipe:0', *protocol, '-i', args.input, '-map', '0:v:0', '-map', '1:a?', '-metadata:s:v:0', 'rotate=0']
    if remove:
        command += ['-c:v', 'qtrle', '-pix_fmt', 'argb', '-c:a', 'pcm_s16le', '-f', 'mov']
    else:
        command += ['-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-f', 'mp4']
        if cover:
            audio_tracks = [stream for stream in metadata['streams'] if stream['codec_type'] == 'audio']
            for index, audio in enumerate(audio_tracks):
                if audio.get('codec_name') == 'aac':
                    command += [f'-c:a:{index}', 'copy']
    writer = spawn(command + ['-threads', '2', args.output], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    write_errors, write_error_thread = drain_errors(writer)
    frames, faces = 0, 0
    while True:
        data = read_exact(reader.stdout, width * height * 3)
        if not data:
            break
        if len(data) != width * height * 3:
            raise RuntimeError('Truncated decoded frame')
        frame = np.frombuffer(data, np.uint8).reshape(height, width, 3).copy()
        if remove:
            tensor = frame.transpose(2, 0, 1)[None].astype(np.float32) / 255
            values = {'src': tensor, 'downsample_ratio': ratio}
            values.update({f'r{i+1}i': state for i, state in enumerate(states)})
            foreground, alpha, *states = session.run(None, values)
            rgb = np.clip(foreground[0].transpose(1, 2, 0) * 255, 0, 255).astype(np.uint8)
            matte = np.clip(alpha[0, 0] * 255, 0, 255).astype(np.uint8)
            frame = np.dstack((rgb, matte))
        elif cover:
            frame = cv2.cvtColor(face_cover.apply(cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)), cv2.COLOR_BGR2RGB)
        else:
            boxes = cascade.detectMultiScale(cv2.cvtColor(frame, cv2.COLOR_RGB2GRAY), 1.15, 5, minSize=(40, 40))
            for x, y, w, h in boxes:
                faces += 1
                roi = frame[y:y+h, x:x+w]
                softened = cv2.bilateralFilter(roi, 7, 28, 28)
                mask = np.zeros((h, w), np.float32)
                cv2.ellipse(mask, (w//2, h//2), (int(w*.43), int(h*.46)), 0, 0, 360, .45, -1)
                mask = cv2.GaussianBlur(mask, (0, 0), max(1, w*.04))[..., None]
                roi[:] = np.clip(roi*(1-mask) + (softened.astype(np.float32)*1.025+1)*mask, 0, 255).astype(np.uint8)
        writer.stdin.write(frame.tobytes())
        frames += 1
        if frames % max(1, round(fps)) == 0:
            emit(progress=min(99, frames / fps / duration * 100) if duration else 0)
    writer.stdin.close()
    read_code, write_code = reader.wait(), writer.wait()
    read_error_thread.join()
    write_error_thread.join()
    if read_code or write_code or not frames:
        raise RuntimeError((read_errors + write_errors).decode('utf8', errors='replace')[-1800:])
    if cover:
        return 'Local facial landmarks drive the shaded cartoon mouth' if args.operation == 'animate_face' else 'Local single face tracking and cartoon cover; original audio retained'
    if not remove and not faces:
        raise ValueError('No frontal face detected; no face smoothing result produced')
    return 'RVM recurrent human matting; alpha qtrle MOV; audio preserved as PCM' if remove else 'Haar face detection with traditional local masked skin smoothing (not generative AI)'


def separate(args, duration):
    import numpy as np
    model = Path(args.model_root) / '2stems'
    required = ('model.index', 'model.data-00000-of-00001', 'checkpoint')
    if not all(workspace_path(model / name).is_file() for name in required):
        raise ValueError('Spleeter model missing: run setup-local-ai.ps1 first')
    if not args.background_output:
        raise ValueError('separate_vocals requires --background-output')
    emit(progress=1, message='Loading local Spleeter model; first CPU startup can take time')
    # A local provider prevents Spleeter from downloading at inference time.
    with redirect_stdout(sys.stderr):
        from spleeter.model.provider import ModelProvider
    class LocalProvider(ModelProvider):
        def get(self, model_directory):
            return str(model)
        def download(self, *unused):
            raise RuntimeError('Runtime model download forbidden')
    ModelProvider.default = classmethod(lambda cls: LocalProvider())
    with redirect_stdout(sys.stderr):
        from spleeter.separator import Separator
        separator = Separator('spleeter:2stems', multiprocess=False)
    handles = []
    try:
        for path in (args.output, args.background_output):
            handle = wave.open(path, 'wb')
            handle.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            handles.append(handle)
        position, step, context = 0, 15, 1
        while position < duration:
            start = max(0, position-context)
            length = min(duration-start, step + context + position-start)
            raw = checked(ffbase(args) + ['-ss', str(start), '-i', args.input, '-t', str(length), '-map', '0:a:0', '-ar', '44100', '-ac', '2', '-f', 'f32le', 'pipe:1'])
            samples = np.frombuffer(raw, '<f4').reshape(-1, 2)
            if not len(samples):
                raise ValueError('No decoded audio samples')
            with redirect_stdout(sys.stderr):
                prediction = separator.separate(samples)
            offset = round((position-start)*44100)
            count = min(round(min(step, duration-position)*44100), len(samples)-offset)
            for handle, name in zip(handles, ('vocals', 'accompaniment')):
                audio = prediction[name][offset:offset+count]
                handle.writeframes(np.clip(audio*32767, -32768, 32767).astype('<i2').tobytes())
            position += step
            emit(progress=min(99, position/duration*100))
    finally:
        for handle in handles:
            handle.close()
    return 'Spleeter 2-stem neural separation; vocals and accompaniment WAV (model bandwidth 11 kHz)'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--operation', required=True, choices=OPERATIONS)
    for name in ('input', 'output', 'ffmpeg', 'ffprobe', 'model-root', 'runtime-root'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--background-output')
    parser.add_argument('--sticker')
    args = parser.parse_args()
    outputs = []
    photo_module = None
    try:
        runtime = workspace_path(args.runtime_root)
        workspace_path(args.model_root)
        if args.operation in FACE_OPERATIONS:
            module_name = 'animated_face' if args.operation == 'animate_face' else 'retouch_photo'
            spec = importlib.util.spec_from_file_location(module_name, Path(__file__).with_name(module_name + '.py'))
            photo_module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(photo_module)
        for name in ('input', 'output', 'ffmpeg', 'ffprobe', 'model_root', 'runtime_root'):
            if not Path(getattr(args, name)).is_absolute():
                raise ValueError(name + ' must be absolute')
        for value in (args.output, args.background_output):
            if value:
                validate_output(args.input, value)
                if any(Path(value).resolve() == Path(existing) for existing in outputs):
                    raise ValueError('Output paths must be distinct')
                outputs.append(str(Path(value).resolve()))
        for directory in ('tmp', 'cache/keras', 'cache/torch'):
            workspace_path(runtime / directory)
        (runtime / 'tmp').mkdir(parents=True, exist_ok=True)
        for key in ('TMP', 'TEMP', 'TMPDIR'):
            os.environ[key] = str(runtime / 'tmp')
        os.environ.update(OMP_NUM_THREADS='2', OPENBLAS_NUM_THREADS='2', TF_NUM_INTRAOP_THREADS='2', TF_NUM_INTEROP_THREADS='1', TF_CPP_MIN_LOG_LEVEL='2', CUDA_VISIBLE_DEVICES='-1', MODEL_PATH=args.model_root,
                          KERAS_HOME=str(runtime / 'cache' / 'keras'), XDG_CACHE_HOME=str(runtime / 'cache'),
                          TORCH_HOME=str(runtime / 'cache' / 'torch'))
        metadata = probe(args, args.input)
        if args.operation in ('remove_background', 'enhance_video', 'smooth_skin', *FACE_OPERATIONS):
            video = next((s for s in metadata['streams'] if s['codec_type'] == 'video'), None)
            if not video:
                raise ValueError('Input has no video stream')
            video_geometry(video)
        if any(abs(float(stream.get('start_time', 0))) > .001 for stream in metadata['streams']
               if stream['codec_type'] in ('audio', 'video')):
            raise ValueError('Nonzero audio/video source offset is not supported; refusing to shift synchronization')
        duration = float(metadata['format'].get('duration', 0))
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError('Input has no finite positive duration')
        if args.operation in ('denoise_audio', 'separate_vocals'):
            audio = next((s for s in metadata['streams'] if s['codec_type'] == 'audio'), None)
            if not audio:
                raise ValueError('Input has no audio stream')
            if duration - float(audio.get('duration', duration)) > .05:
                raise ValueError('Audio duration is shorter than source clock; refusing to truncate source duration')
        emit(progress=0, message='Processing locally on CPU')
        if args.operation in ('denoise_audio', 'enhance_video'):
            message = traditional(args, duration, metadata)
        elif args.operation == 'separate_vocals':
            message = separate(args, float(audio.get('duration', duration)))
        else:
            message = video_frames(args, metadata, duration, photo_module)
        if any(not Path(value).is_file() or Path(value).stat().st_size == 0 for value in outputs):
            raise RuntimeError('Processing produced empty output')
        for value in outputs:
            if float(probe(args, value)['format'].get('duration', 0)) <= 0:
                raise RuntimeError('Processing produced no playable duration')
        emit(progress=100, ok=True, path=args.output, backgroundPath=args.background_output or '', message=message)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        for child in CHILDREN:
            if child.poll() is None:
                child.kill()
            child.wait()
        for value in outputs:
            Path(value).unlink(missing_ok=True)
        if args.operation in FACE_OPERATIONS:
            error_type = getattr(photo_module, 'PhotoError', ())
            code = 'workspace_not_configured' if isinstance(error, WorkspaceNotConfigured) else 'local_process_failed'
            emit(ok=False, path='', error=str(error) if isinstance(error, error_type) else code)
        else:
            emit(ok=False, path='', message=str(error) or 'Cancelled')
        return 1


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    sys.exit(main())
