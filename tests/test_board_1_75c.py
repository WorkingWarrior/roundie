"""Host tests of the ESP32-S3-Touch-AMOLED-1.75C board's hardware-independent logic."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'components/roundie_board/boards/waveshare_s3_touch_amoled_1_75c'


class Board175cTests(unittest.TestCase):
    def test_touch_filter_and_pmic_decode(self):
        with tempfile.TemporaryDirectory() as folder:
            for standard in ('c11', 'c23'):
                output = str(Path(folder) / f'board-{standard}')
                subprocess.run(['cc', f'-std={standard}', '-Wall', '-Wextra', '-Werror',
                                '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                '-I' + BOARD, BOARD + '/board_logic.c', 'tests/test_board_1_75c.c',
                                '-o', output], cwd=ROOT, check=True, capture_output=True)
                subprocess.run([output], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
