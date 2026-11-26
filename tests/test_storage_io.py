"""io_worker hand-over, OTA/package claim and bounded I/O (docs/storage-io.md, P1)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "components/roundie_storage"
sys.path.insert(0, str(ROOT / "tools"))

import roundie_elf  # noqa: E402
import roundie_face  # noqa: E402
import roundie_package  # noqa: E402
from roundie_time import DeviceBusy  # noqa: E402


def compile_and_run(test, *sources, defines=()):
    with tempfile.TemporaryDirectory() as temporary:
        executable = Path(temporary) / "test"
        subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-pthread",
                        "-I" + str(STORAGE / "include"),
                        "-I" + str(ROOT / "components/roundie_platform/include"),
                        "-I" + str(ROOT / "platform/linux/include"),
                        *defines, str(ROOT / "tests" / test), *map(str, sources),
                        "-o", str(executable)], check=True)
        return subprocess.run([str(executable)], check=True, timeout=20,
                              capture_output=True, text=True).stdout


class StorageIoTests(unittest.TestCase):
    def test_forward_busy_and_claim(self):
        output = compile_and_run("test_storage_io.c", STORAGE / "io_forward.c", STORAGE / "claim.c")
        self.assertIn("storage io: PASS", output)

    def test_hosts_recognize_busy(self):
        for module, prefix in ((roundie_package, "RP1"), (roundie_elf, "RE1"), (roundie_face, "RF1")):
            with self.subTest(prefix=prefix):
                line = f'\n!{prefix} 12345678 259 {{"format":1,"error":"busy"}}\n'.encode()[1:]
                with self.assertRaises(DeviceBusy):
                    module.parse_reply(line, "12345678")


if __name__ == "__main__":
    unittest.main()
