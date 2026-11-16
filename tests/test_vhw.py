"""The desktop's virtual hardware model (platform/linux/vhw.c) under ASan/UBSan and TSan; no SDL or hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def build(directory, name, sanitize):
    executable = str(Path(directory) / name)
    subprocess.run([
        "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
        "-fsanitize=" + sanitize,
        "-I" + str(ROOT / "platform/linux"),
        "-I" + str(ROOT / "platform/linux/include"),
        "-I" + str(ROOT / "components/roundie_board/include"),
        str(ROOT / "tests/test_vhw.c"), str(ROOT / "platform/linux/vhw.c"), "-o", executable,
    ], check=True)
    return executable


class VirtualHardwareTests(unittest.TestCase):
    def test_model(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([build(directory, "vhw", "address,undefined")], check=True, timeout=30)

    def test_model_threads(self):
        """The same test with a second thread reading the machine throughout."""
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([build(directory, "vhw-tsan", "thread"), "--reader"],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("ThreadSanitizer", result.stderr)


if __name__ == "__main__":
    unittest.main()
