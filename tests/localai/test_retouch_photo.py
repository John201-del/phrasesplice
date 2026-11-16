"""Check the PNG workflow with real photos, pixels and the installed matte model."""
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
WORK = DATA / 'tests'
PORTRAIT = WORK / "avatar" / "portrait.png"
WORKER = ROOT / "data/scripts/local_ai/retouch_photo.py"
MODELS = DATA / "local-ai/models"
RUNTIME = DATA / "local-ai"
MEDIA = ROOT / "data/scripts/local_ai/process_media.py"
FFMPEG = ROOT / "runtime/bin/ffmpeg.exe"
FFPROBE = ROOT / "runtime/bin/ffprobe.exe"


def pixels(path):
    return cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_UNCHANGED)


@unittest.skipUnless(PORTRAIT.is_file() and FFMPEG.is_file() and (MODELS / 'rvm_mobilenetv3_fp32.onnx').is_file(),
                     'Requires optional local AI runtime and portrait fixture in the selected workspace')
class RetouchPhotoTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="photo-test-", dir=WORK)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.original = PORTRAIT.read_bytes()
        self.photo = pixels(PORTRAIT)

    def run_worker(self, options, *, source=PORTRAIT, existing=b""):
        output = self.folder / "修图验收.png"
        output.write_bytes(existing)
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(WORKER), "--input", str(source), "--output", str(output),
             "--model-root", str(MODELS), "--options", json.dumps(options)],
            capture_output=True, text=True, encoding="utf-8", timeout=60,
        )
        self.assertEqual(PORTRAIT.read_bytes(), self.original)
        return result, output

    def check_png(self, result, output, channels):
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        reply = json.loads(result.stdout.splitlines()[-1])
        self.assertTrue(reply["ok"])
        self.assertEqual(Path(reply["path"]), output)
        image = pixels(output)
        self.assertEqual(image.shape, (*self.photo.shape[:2], channels))
        return image

    def test_portrait_smoothing_keeps_background(self):
        result, output = self.run_worker({"skinStrength": 55})
        edited = self.check_png(result, output, 3)
        self.assertGreater(np.count_nonzero(edited != self.photo), 40)
        np.testing.assert_array_equal(edited[-100:], self.photo[-100:])
        self.assertLess(np.abs(edited.astype(np.int16) - self.photo).mean(), 4)

    def test_repairs_only_marked_spot_on_unicode_path(self):
        marked = self.photo.copy()
        h, w = marked.shape[:2]
        x, y = w // 2, h // 2
        marked[y-2:y+3, x-2:x+3] = (255, 0, 255)
        source = self.folder / "带斑点的照片.png"
        cv2.imencode(".png", marked)[1].tofile(source)
        result, output = self.run_worker(
            {"blemishes": [{"xPercent": 50, "yPercent": 50, "radiusPercent": 2}]}, source=source,
        )
        edited = self.check_png(result, output, 3)
        self.assertFalse(np.array_equal(edited[y, x], marked[y, x]))
        changes = np.any(edited != marked, axis=2)
        yy, xx = np.ogrid[:h, :w]
        self.assertFalse(np.any(changes & ((xx - x)**2 + (yy - y)**2 > (min(h, w) * .025)**2)))
        np.testing.assert_array_equal(pixels(source), marked)

    def test_person_matte_preserves_alpha(self):
        result, output = self.run_worker({"background": {"mode": "transparent"}})
        edited = self.check_png(result, output, 4)
        self.assertLess(int(edited[2, 2, 3]), 10)
        self.assertGreater(int(edited[self.photo.shape[0]//2, self.photo.shape[1]//2, 3]), 240)
        self.assertGreater(np.count_nonzero((edited[:, :, 3] > 10) & (edited[:, :, 3] < 245)), 30)

    def test_combines_skin_with_replacement_color(self):
        result, output = self.run_worker({"skinStrength": 30, "background": {"mode": "color", "color": "#2468AC"}})
        edited = self.check_png(result, output, 3)
        np.testing.assert_allclose(edited[2, 2], (172, 104, 36), atol=4)
        self.assertGreater(np.std(edited[125:260, 130:310]), 15)

    def test_replaces_with_local_photo(self):
        background = self.folder / "背景.png"
        pattern = np.full((100, 220, 3), (120, 64, 28), np.uint8)
        cv2.imencode(".png", pattern)[1].tofile(background)
        result, output = self.run_worker({"background": {"mode": "image", "path": str(background)}})
        edited = self.check_png(result, output, 3)
        np.testing.assert_allclose(edited[2, 2], (120, 64, 28), atol=4)
        np.testing.assert_array_equal(pixels(background), pattern)

    def test_rotated_jpeg_keeps_display_dimensions(self):
        path = self.folder / "带方向的照片.jpg"
        cropped = self.photo[:300, :400]
        jpeg = cv2.imencode(".jpg", cropped)[1].tobytes()
        exif = b"Exif\0\0II" + struct.pack("<HIHHHIHI", 42, 8, 1, 274, 3, 1, 6, 0) + b"\0\0"
        path.write_bytes(jpeg[:2] + b"\xff\xe1" + struct.pack(">H", len(exif)+2) + exif + jpeg[2:])
        original = path.read_bytes()
        result, output = self.run_worker({"blemishes": [{"xPercent": 50, "yPercent": 50, "radiusPercent": 1}]}, source=path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(pixels(output).shape, (400, 300, 3))
        self.assertEqual(path.read_bytes(), original)

    def test_rejects_unknown_location_and_keeps_existing_output(self):
        result, output = self.run_worker({"blemishes": [{"xPercent": 123, "yPercent": 50, "radiusPercent": 1}]})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.stat().st_size, 0)
        result, output = self.run_worker({"skinStrength": 20}, existing=b"must keep")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b"must keep")

    def sticker(self):
        path = self.folder / "卡通头像.png"
        overlay = np.zeros((96, 96, 4), np.uint8)
        cv2.circle(overlay, (48, 48), 42, (239, 23, 228, 255), -1)
        cv2.imencode(".png", overlay)[1].tofile(path)
        return path

    def test_covers_single_face_on_photo(self):
        result, output = self.run_worker({"stickerPath": str(self.sticker())})
        edited = self.check_png(result, output, 3)
        mask = (edited[:, :, 0] > 210) & (edited[:, :, 1] < 50) & (edited[:, :, 2] > 200)
        self.assertGreater(np.count_nonzero(mask[:240]), 1000)
        np.testing.assert_array_equal(edited[-100:], self.photo[-100:])

    def make_motion(self, *, lost=False):
        tile = cv2.resize(self.photo, (320, 320), interpolation=cv2.INTER_AREA)
        for index in range(12):
            frame = np.zeros((320, 480, 3), np.uint8)
            x = 8 * index
            if not lost or index != 6:
                frame[:, x:x+320] = tile
            cv2.imencode(".png", frame)[1].tofile(self.folder / f"frame{index:02d}.png")
        source = self.folder / "移动车脸.mp4"
        subprocess.run([str(FFMPEG), "-v", "error", "-y", "-framerate", "12", "-i", str(self.folder / "frame%02d.png"),
                        "-f", "lavfi", "-i", "sine=frequency=500:duration=1", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                        "-c:a", "aac", "-shortest", str(source)], check=True, timeout=30)
        return source

    def cover_video(self, source):
        output = self.folder / "遮挡后.mp4"
        original = source.read_bytes()
        result = subprocess.run([sys.executable, "-I", "-B", str(MEDIA), "--operation", "cover_face", "--input", str(source),
                                 "--output", str(output), "--ffmpeg", str(FFMPEG), "--ffprobe", str(FFPROBE),
                                 "--runtime-root", str(RUNTIME), "--model-root", str(MODELS), "--sticker", str(self.sticker())],
                                capture_output=True, text=True, encoding="utf-8", timeout=60)
        self.assertEqual(source.read_bytes(), original)
        return result, output

    def test_video_sticker_moves_and_audio_is_kept(self):
        source = self.make_motion()
        result, output = self.cover_video(source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        def metadata(path):
            return json.loads(subprocess.check_output([str(FFPROBE), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)]))
        original, exported = metadata(source), metadata(output)
        video, audio = exported["streams"]
        self.assertEqual((video["width"], video["height"], video["nb_frames"], audio["codec_name"]), (480, 320, "12", "aac"))
        self.assertEqual(exported["format"]["duration"], original["format"]["duration"])
        self.assertEqual(audio["duration"], original["streams"][1]["duration"])
        def audio_payload(path):
            return subprocess.check_output([str(FFMPEG), "-v", "error", "-i", str(path), "-map", "0:a:0", "-c", "copy", "-f", "adts", "-"])
        self.assertEqual(audio_payload(output), audio_payload(source))
        raw = subprocess.check_output([str(FFMPEG), "-v", "error", "-i", str(output), "-f", "rawvideo", "-pix_fmt", "bgr24", "-"])
        frames = np.frombuffer(raw, np.uint8).reshape(12, 320, 480, 3)
        centers = []
        for frame in frames:
            b, g, r = cv2.split(frame)
            _, xs = np.where((b > 190) & (r > 175) & (g < 72))
            self.assertGreater(xs.size, 900)
            centers.append(xs.mean())
        self.assertGreater(centers[-1] - centers[0], 70)
        self.assertLess(centers[-1] - centers[0], 103)

    def test_video_aborts_if_a_face_disappears(self):
        result, output = self.cover_video(self.make_motion(lost=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout.splitlines()[-1])["error"], "face_tracking_lost")
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
