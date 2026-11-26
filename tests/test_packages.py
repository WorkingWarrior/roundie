"""Real package format, POSIX manager and validator with ASan/UBSan."""
from pathlib import Path
import os
import json
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import roundie_package

from xtensa_toolchain import TOOLCHAIN


SHORT_LIMIT = 700


class PackageTests(unittest.TestCase):
    def run_production(self, vfs):
        """Builds the example packages and the host harness; vfs substitutes
        the VFS under bounded_io.c (short reads/writes, EOF in a chunk)."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            env = dict(os.environ, PATH=str(TOOLCHAIN) + os.pathsep + os.environ["PATH"])
            packages = []
            for example, stem, app_id, name in (
                    ("hello_elf", "hello", "org.roundie.hello", "Hello"),
                    ("event_elf", "event", "org.roundie.events", "Events"),
                    ("ui_elf", "ui", "org.roundie.ui", 'UI "Próba"'),
                    ("stopwatch_elf", "stopwatch", "org.roundie.stopwatch", "Stopwatch")):
                build = directory / example
                subprocess.run(["cmake", "-S", str(ROOT / "examples" / example), "-B", str(build)],
                               env=env, check=True, capture_output=True)
                subprocess.run(["cmake", "--build", str(build)], env=env, check=True, capture_output=True)
                package = directory / (stem + ".rpk")
                first = roundie_package.build(build / (stem + ".app.elf"), package, app_id, name, "1.0.0")
                repeat = directory / (stem + "-repeat.rpk")
                second = roundie_package.build(build / (stem + ".app.elf"), repeat, app_id, name, "1.0.0")
                self.assertEqual(package.read_bytes(), repeat.read_bytes())
                self.assertEqual(first, second)
                packages.append(str(package))
            executable = directory / "tests"
            defines = []
            sources = []
            if vfs:
                defines = ["-DROUNDIE_IO_READ=fake_read", "-DROUNDIE_IO_WRITE=fake_write",
                           "-DFAKE_VFS_SHORT=" + str(SHORT_LIMIT)]
                sources = [str(ROOT / "tests/fake_vfs.c")]
            subprocess.run(["cc", "-std=gnu2x", "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-I" + str(ROOT / "tests/elf_mocks"),
                            "-I" + str(ROOT / "tests"),
                            "-I" + str(ROOT / "tests/ota_mocks"), "-I" + str(ROOT / "include"),
                            "-I" + str(ROOT / "components/roundie_elf/include"),
                            "-I" + str(ROOT / "components/roundie_packages/include"),
                            "-I" + str(ROOT / "components/roundie_storage/include"),
                            '-DPACKAGES_DIRECTORY="' + str(directory / "packages") + '"', *defines,
                            str(ROOT / "tests/test_packages.c"),
                            str(ROOT / "components/roundie_packages/format.c"),
                            str(ROOT / "components/roundie_elf/validate.c"),
                            str(ROOT / "components/roundie_storage/bounded_io.c"), *sources,
                            "-lcrypto", "-o", str(executable)], check=True)
            # The harness takes the first three packages; the fourth (ELF larger
            # than one 4 KiB chunk) is for the substituted-VFS cases only.
            subprocess.run([str(executable), *(packages if vfs else packages[:3])], check=True, timeout=60)

    def test_production_transaction_registry_and_format(self):
        self.run_production(vfs=False)

    def test_short_reads_and_mid_chunk_eof_in_bounded_loops(self):
        """Whole harness with every bounded call cut to SHORT_LIMIT bytes, then
        EOF/zero-write injected inside a 4 KiB chunk of the hash loop, ELF
        load, app data read and app data write."""
        self.run_production(vfs=True)

    def test_version_identity_and_invalid_host_input(self):
        for value in ("a..b", "../hello", "org.Roundie", "org.1", "a.b/evil", "a." + "x" * 63):
            with self.assertRaises(roundie_package.DeviceError):
                roundie_package.valid_id(value)

    def test_bounded_host_reply_validation(self):
        response = dict(format=1, allowed=1, busy=0, phase=0, session="00000000",
                        received=0, total=0, count=1, orphans=0, capacity=[1, 9240576, 24576],
                        us=[0] * 7, error="", apps=[["org.roundie.hello", "1.0.0", 1, 0]],
                        name='Próba "UTF-8"', sha256="", elf_size=1692)
        def frame(value, code=0):
            return f"!RP1 12345678 {code} {json.dumps(value)}\n".encode()
        self.assertEqual(roundie_package.parse_reply(frame(response), "12345678"), response)
        self.assertIsNone(roundie_package.parse_reply(frame(response), "87654321"))
        for key, value in (("phase", 6), ("total", roundie_package.MAX_FILE + 1),
                           ("session", "oops"), ("count", 9), ("us", [0]),
                           ("apps", [["../bad", "1.0.0", 1, 0]]),
                           ("apps", response["apps"] * 2), ("sha256", "bad")):
            with self.subTest(key=key, value=value):
                changed = dict(response, **{key: value})
                with self.assertRaises(roundie_package.TransportError):
                    roundie_package.parse_reply(frame(changed), "12345678")
        with self.assertRaises(roundie_package.DeviceError):
            roundie_package.parse_reply(frame(response, 258), "12345678")


if __name__ == "__main__":
    unittest.main()
