"""Diagnostic boot history ring; NVS is mocked."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootHistoryTests(unittest.TestCase):
    def test_ring(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "history")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "tests/recovery_mocks"),
                "-I" + str(ROOT / "tests/ota_mocks"),
                "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                str(ROOT / "tests/test_boot_history.c"),
                str(ROOT / "components/roundie_boot_guard/history.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
