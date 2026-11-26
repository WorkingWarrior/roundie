"""Actual engine/persistence/recovery controller; IDF and transport are mocked."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BootGuardTests(unittest.TestCase):
    def test_normal_and_development_engines(self):
        """Every engine variant, also with every injected cause-record fault.

        The latch trace (every latch record written, in order) must be the same
        with and without a failing diagnostic write: the Safe Mode/BootGuard
        logic does not depend on the cause record."""
        with tempfile.TemporaryDirectory() as directory:
            for diagnostic in (0, 1):
                traces = {}
                for fault in (0, 1, 2, 3):
                    traces[fault] = self.run_variant(directory, diagnostic, fault)
                self.assertIn("latch trace: ", traces[0])
                for fault in (1, 2, 3):
                    self.assertEqual(traces[0], traces[fault], f"diagnostic={diagnostic} fault={fault}")

    def run_variant(self, directory, diagnostic, fault):
                executable = str(Path(directory) / f"boot-{diagnostic}-{fault}")
                subprocess.run([
                    "cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", f"-DCONFIG_ROUNDIE_BOOT_GUARD_TEST={diagnostic}", f"-DCAUSE_FAULT={fault}",
                    "-DCONFIG_ROUNDIE_APP_RUNTIME=1",
                    "-I" + str(ROOT / "tests/recovery_mocks"),
                    "-I" + str(ROOT / "tests/ota_mocks"),
                    "-I" + str(ROOT / "tests/elf_mocks"),
                    "-I" + str(ROOT / "components/roundie_elf/include"),
                    "-I" + str(ROOT / "components/roundie_boot_guard/include"),
                    "-I" + str(ROOT / "components/roundie_ota/include"),
                    "-I" + str(ROOT / "components/roundie_storage_mode/include"),
                    "-I" + str(ROOT / "components/roundie_storage/include"),
                    "-I" + str(ROOT / "main"), str(ROOT / "tests/test_boot_guard.c"),
                    str(ROOT / "components/roundie_boot_guard/persistence.c"),
                    str(ROOT / "components/roundie_boot_guard/diagnostics.c"),
                    str(ROOT / "components/roundie_storage_mode/storage_mode.c"),
                    str(ROOT / "main/recovery_diagnostics.c"), str(ROOT / "tests/io_worker_stub.c"),
                    str(ROOT / "components/roundie_elf/crash.c"), "-o", executable,
                ], check=True)
                result = subprocess.run([executable], check=True, timeout=10, capture_output=True, text=True)
                return next(line for line in result.stdout.splitlines() if line.startswith("latch trace: "))

if __name__ == "__main__":
    unittest.main()
