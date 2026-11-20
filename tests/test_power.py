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

    def lifecycle(self, sanitizer):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "lifecycle")
            subprocess.run([
                "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
                "-fsanitize=" + sanitizer,
                "-I" + str(ROOT / "components/roundie_power/include"),
                str(ROOT / "tests/test_power_lifecycle.c"),
                str(ROOT / "components/roundie_power/power.c"),
                str(ROOT / "components/roundie_power/inhibit.c"),
                str(ROOT / "components/roundie_power/lifecycle.c"), "-o", executable,
            ], check=True)
            result = subprocess.run([executable], check=True, timeout=60, capture_output=True, text=True)
            self.assertIn("Power state model and inhibitors PASS", result.stdout)

    def test_lifecycle_and_inhibitors(self):
        """M8-B state model and inhibitors under ASan/UBSan."""
        self.lifecycle("address,undefined")

    def test_lifecycle_and_inhibitors_tsan(self):
        """The same, its threaded case under TSan."""
        self.lifecycle("thread")

if __name__ == "__main__":
    unittest.main()
