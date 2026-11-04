"""Actual tone generator of the audio component under ASan/UBSan; no IDF or hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ToneTests(unittest.TestCase):
    def test_tone_generator(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "tone")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/roundie_audio/include"),
                str(ROOT / "tests/test_tone.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
