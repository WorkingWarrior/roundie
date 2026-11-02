"""Actual engine/persistence/recovery controller; IDF and transport are mocked."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BootGuardTests(unittest.TestCase):
    def test_normal_and_development_engines(self):
        with tempfile.TemporaryDirectory() as directory:
            for diagnostic in (0, 1):
                executable = str(Path(directory) / f"boot-{diagnostic}")
                subprocess.run([
                    "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", f"-DCONFIG_ROUNDIE_BOOT_GUARD_TEST={diagnostic}",
                    "-DCONFIG_ROUNDIE_APP_RUNTIME=1",
                    "-I" + str(ROOT / "tests/recovery_mocks"),
                    "-I" + str(ROOT / "tests/ota_mocks"),
                    "-I" + str(ROOT / "tests/elf_mocks"),
                    "-I" + str(ROOT / "components/roundie_elf/include"),
                    "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                    "-I" + str(ROOT / "components/roundie_ota/include"),
                    "-I" + str(ROOT / "main"), str(ROOT / "tests/test_boot_guard.c"),
                    str(ROOT / "components/roundie_boot_guard/persistence.c"),
                    str(ROOT / "main/recovery_diagnostics.c"), "-o", executable,
                ], check=True)
                subprocess.run([executable], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
