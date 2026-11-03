"""Actual PowerManager policy under ASan/UBSan; no IDF or hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PowerPolicyTests(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "power")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                "-fsanitize=address,undefined",
                "-I" + str(ROOT / "components/roundie_power/include"),
                str(ROOT / "tests/test_power.c"),
                str(ROOT / "components/roundie_power/power.c"), "-o", executable,
            ], check=True)
            subprocess.run([executable], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
