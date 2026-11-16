"""Real pinned LVGL + production bridge/runtime; mocked IDF/loader, ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
from xtensa_toolchain import TOOLCHAIN


class AppUiTests(unittest.TestCase):
    def test_production_ui_and_runtime(self):
        with tempfile.TemporaryDirectory(prefix="roundie-ui-test-") as temporary:
            build = Path(temporary)
            env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
            examples = []
            for name, binary in (("hello_elf", "hello"), ("event_elf", "event"), ("ui_elf", "ui"),
                                 ("round_elf", "round"), ("watch_elf", "watch"),
                                 ("stopwatch_elf", "stopwatch")):
                target = build / name
                subprocess.run(["cmake", "-S", str(ROOT / "examples" / name), "-B", str(target)],
                               check=True, capture_output=True, env=env)
                subprocess.run(["cmake", "--build", str(target)], check=True, capture_output=True, env=env)
                examples.append(str(target / f"{binary}.app.elf"))
            subprocess.run(["cmake", "-S", str(ROOT / "tests/app_ui"), "-B", str(build / "host"),
                            "-G", "Ninja"], check=True, capture_output=True)
            subprocess.run(["cmake", "--build", str(build / "host"), "-j", "8"],
                           check=True, capture_output=True)
            subprocess.run([str(build / "host/ui_tests"), *examples[:3]], check=True, timeout=20)
            subprocess.run([str(build / "host/round_tests"), examples[3]], check=True, timeout=20)
            subprocess.run([str(build / "host/launcher_tests")], check=True, timeout=20)
            subprocess.run([str(build / "host/launcher_tests"), "low-memory"], check=True, timeout=20)
            subprocess.run([str(build / "host/display_tests")], check=True, timeout=20)
