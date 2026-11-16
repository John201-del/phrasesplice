import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest


class TranscriptWordsTest(unittest.TestCase):
    def test_requests_word_timing_and_writes_utf8_sidecar(self):
        calls = []
        fake = types.ModuleType("whispertotext")

        def transcribe(source, model, device, task, arguments):
            calls.append(arguments)
            Path(source).with_suffix(".srt").write_text("字幕", encoding="utf-8")
            return {"segments": [{"words": [{"start": 0.25, "end": 0.8, "word": "你好"}]}]}

        fake.run_whisper = transcribe
        previous = sys.modules.get("whispertotext")
        sys.modules["whispertotext"] = fake
        try:
            script = Path(__file__).resolve().parents[2] / "data/scripts/whisper/whispertosrt.py"
            spec = importlib.util.spec_from_file_location("tested_whispertosrt", script)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with tempfile.TemporaryDirectory() as folder:
                source = Path(folder) / "audio.wav"
                module.main(str(source), "small", word_timestamps="True", fp16="False")
                words = json.loads(source.with_suffix(".words.json").read_text(encoding="utf-8"))
                self.assertEqual(words, [{"start": 0.25, "end": 0.8, "text": "你好"}])
                self.assertIn("word_timestamps=True", calls[0])
                self.assertEqual(source.with_suffix(".srt").read_text(encoding="utf-8"), "字幕")
        finally:
            if previous is None:
                sys.modules.pop("whispertotext", None)
            else:
                sys.modules["whispertotext"] = previous


if __name__ == "__main__":
    unittest.main()
