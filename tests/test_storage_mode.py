"""Actual one-shot request and admission logic of USB storage mode under
ASan/UBSan; no IDF or hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class StorageModeTests(unittest.TestCase):
    def test_request_and_admission(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "storage_mode")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/roundie_storage_mode/include"),
                str(ROOT / "tests/test_storage_mode.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_session_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "storage_session")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/roundie_storage_mode/include"),
                str(ROOT / "tests/test_storage_session.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
