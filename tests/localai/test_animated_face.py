"""Verify the animated character on a real local speaking-face fixture."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
DATA = Path(os.environ['LOCALCUT_WORKSPACE_ROOT'])
WORK = DATA / 'tests'
RUNTIME = DATA / 'local-avatar'
FFMPEG = ROOT / "runtime/bin/ffmpeg.exe"
FFPROBE = ROOT / "runtime/bin/ffprobe.exe"
SOURCE = WORK / "avatar/speech-result.mp4"


@unittest.skipUnless(SOURCE.is_file() and FFMPEG.is_file() and (RUNTIME / 'models/alignment_WFLW_4HG.pth').is_file(),
                     'Requires optional avatar runtime and speaking-face fixture in the selected workspace')
class AnimatedFaceTest(unittest.TestCase):
    def test_open_and_closed_mouth_follow_the_source_without_reencoding_aac(self):
        original = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
        with tempfile.TemporaryDirectory(prefix="cartoon-face-", dir=WORK) as folder:
            output = Path(folder) / "带口型的立体青蛙.mp4"
            job = subprocess.run(
                [sys.executable, "-I", "-B", str(ROOT / "data/scripts/local_ai/process_media.py"),
                 "--operation", "animate_face", "--input", str(SOURCE), "--output", str(output),
                 "--ffmpeg", str(FFMPEG), "--ffprobe", str(FFPROBE), "--runtime-root", str(RUNTIME),
                 "--model-root", str(RUNTIME / "models")],
                capture_output=True, text=True, encoding="utf-8", timeout=180,
            )
            self.assertEqual(job.returncode, 0, job.stdout + job.stderr)
            self.assertTrue(json.loads(job.stdout.splitlines()[-1])["ok"])
            metadata = json.loads(subprocess.check_output([str(FFPROBE), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(output)]))
            video, audio = metadata["streams"]
            self.assertEqual((video["width"], video["height"], video["nb_frames"], video["duration"], audio["duration"]),
                             (256, 256, "50", "2.000000", "2.000000"))
            def aac(path):
                return subprocess.check_output([str(FFMPEG), "-v", "error", "-i", str(path), "-map", "0:a:0", "-c", "copy", "-f", "adts", "-"])
            self.assertEqual(aac(output), aac(SOURCE))
            raw = subprocess.check_output([str(FFMPEG), "-v", "error", "-i", str(output), "-f", "rawvideo", "-pix_fmt", "bgr24", "-"])
            frames = np.frombuffer(raw, np.uint8).reshape(50, 256, 256, 3)
            areas = []
            for frame in frames:
                b, g, r = (channel.astype(np.int16) for channel in cv2.split(frame[64:238, 36:220]))
                green = (g > 75) & (g > r + 35) & (g > b + 35)
                self.assertGreater(np.count_nonzero(green), 2000)
                self.assertGreater(np.percentile(g[green], 85) - np.percentile(g[green], 15), 25)
                mouth = (r > g + 24) & (b > g + 8) & (r > 42) & (r < 118) & (g < 55)
                areas.append(np.count_nonzero(mouth))
            # The fixture is closed at frame 0 and clearly open during syllables at frames 10 and 35.
            self.assertGreater(min(areas[10], areas[35]), areas[0] + 45, areas)
            self.assertGreater(max(areas) - min(areas), 80)
        self.assertEqual(hashlib.sha256(SOURCE.read_bytes()).hexdigest(), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
