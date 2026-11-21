"""BQ27220 capacity procedure (bq27220_config.c) and restore decision
(gauge_restore.c) against the gauge model in test_bq27220_config.c; capacity
learning tracker, load hold and learned-FCC record (gauge_learning.c)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = "components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b"
SOURCES = [f"{BOARD}/bq27220_config.c", f"{BOARD}/sensor_codec.c", "components/roundie_board/rtc_time.c",
           "components/roundie_board/gauge_restore.c", "components/roundie_board/gauge_learning.c",
           "tests/test_bq27220_config.c"]


LEARNING_SOURCES = ["components/roundie_board/gauge_learning.c", "tests/test_gauge_learning.c"]


class Bq27220Tests(unittest.TestCase):
    def run_host(self, name, sources, marker):
        with tempfile.TemporaryDirectory() as folder:
            for standard in ("c11", "c23"):
                output = str(Path(folder) / f"{name}-{standard}")
                build = subprocess.run(["cc", f"-std={standard}", "-Wall", "-Wextra", "-Werror", "-g",
                                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                        "-fno-omit-frame-pointer", "-Iplatform/linux/include",
                                        "-Icomponents/roundie_board/include", f"-I{BOARD}", *sources,
                                        "-o", output], cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                result = subprocess.run([output], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(marker, result.stdout)

    def test_procedure_on_gauge_model(self):
        self.run_host("bq27220", SOURCES, "bq27220: OK")

    def test_learning_tracker_hold_and_record(self):
        self.run_host("gauge-learning", LEARNING_SOURCES, "gauge learning: OK")


if __name__ == "__main__":
    unittest.main()
