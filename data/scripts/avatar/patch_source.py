"""Reproducible compatibility/security patches for the pinned upstream revision."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_avatar import workspace_path

def patch(source):
    source = workspace_path(source)
    path = workspace_path(source / 'src/facerender/animate.py')
    text = path.read_text(encoding='utf-8')
    line = 'from src.utils.face_enhancer import enhancer_generator_with_len, enhancer_list'
    if '\n' + line in text:
        text = text.replace('\n' + line, '')
        text = text.replace('        if enhancer:\n', '        if enhancer:\n            ' + line + '\n')
    # Keep the model's square frames when a crop touches a photo edge.
    text = text.replace('        if original_size:\n', "        if original_size and preprocess != 'crop':\n")
    path.write_text(text, encoding='utf-8')
    path = workspace_path(source / 'src/face3d/extract_kp_videos_safe.py')
    text = path.read_text(encoding='utf-8')
    text = text.replace("root_path = 'gfpgan/weights'", "root_path = os.environ['AVATAR_MODELS']")
    path.write_text(text, encoding='utf-8')
    path = workspace_path(source / 'src/utils/croper.py')
    text = path.read_text(encoding='utf-8')
    text = text.replace("raise 'can not detect the landmark from source image'", "raise ValueError('AVATAR_NO_FACE')")
    path.write_text(text, encoding='utf-8')
    path = workspace_path(source / 'src/utils/videoio.py')
    text = path.read_text(encoding='utf-8')
    text = text[:text.index('def save_video_with_watermark(')] + '''def save_video_with_watermark(video, audio, save_path, watermark=False):
    import subprocess
    if watermark:
        raise RuntimeError('watermark mode is disabled')
    subprocess.run([os.environ['AVATAR_FFMPEG'], '-nostdin', '-y', '-hide_banner', '-loglevel', 'error', '-i', video, '-i', audio, '-c:v', 'copy', '-c:a', 'aac', '-shortest', save_path], check=True)
'''
    path.write_text(text, encoding='utf-8')

if __name__ == '__main__':
    patch(sys.argv[1])
