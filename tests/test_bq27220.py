"""BQ27220 capacity procedure (bq27220_config.c) and restore decision
(gauge_restore.c) against the gauge model in test_bq27220_config.c."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = "components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b"
SOURCES = [f"{BOARD}/bq27220_config.c", f"{BOARD}/sensor_codec.c", "components/roundie_board/rtc_time.c",
           "components/roundie_board/gauge_restore.c", "tests/test_bq27220_config.c"]


class Bq27220Tests(unittest.TestCase):
    def test_procedure_on_gauge_model(self):
        with tempfile.TemporaryDirectory() as folder:
            for standard in ("c11", "c23"):
                output = str(Path(folder) / f"bq27220-{standard}")
                subprocess.run(["cc", f"-std={standard}", "-Wall", "-Wextra", "-Werror", "-g",
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-Iplatform/linux/include",
                                "-Icomponents/roundie_board/include", f"-I{BOARD}", *SOURCES, "-o", output],
                               cwd=ROOT, check=True, capture_output=True)
                result = subprocess.run([output], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("bq27220: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
