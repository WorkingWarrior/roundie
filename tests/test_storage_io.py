"""io_worker hand-over, OTA/package claim and bounded I/O (docs/storage-io.md, P1)."""
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "components/roundie_storage"
sys.path.insert(0, str(ROOT / "tools"))

import roundie_elf  # noqa: E402
import roundie_face  # noqa: E402
import roundie_package  # noqa: E402
sys.path.insert(0, str(ROOT / "tools/ci"))
import check_storage_vfs  # noqa: E402
from roundie_time import BUSY_ATTEMPTS, BUSY_CODE, DeviceBusy, DeviceError, TransportError  # noqa: E402


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

    def test_bounded_io_fake_vfs_and_guard(self):
        output = compile_and_run("test_storage_guard.c", STORAGE / "bounded_io.c", STORAGE / "claim.c",
                                 defines=("-DROUNDIE_IO_READ=fake_read", "-DROUNDIE_IO_WRITE=fake_write"))
        self.assertIn("storage guard: PASS", output)

    def test_hosts_recognize_busy(self):
        """P1 fix 3: only 503 with the BUSY payload is DeviceBusy; the payload under 259 is malformed."""
        for module, prefix in ((roundie_package, "RP1"), (roundie_elf, "RE1"), (roundie_face, "RF1")):
            with self.subTest(prefix=prefix):
                line = f'!{prefix} 12345678 503 {{"format":1,"error":"busy"}}\n'.encode()
                with self.assertRaises(DeviceBusy):
                    module.parse_reply(line, "12345678")
                line = f'!{prefix} 12345678 259 {{"format":1,"error":"busy"}}\n'.encode()
                with self.assertRaises(TransportError):
                    module.parse_reply(line, "12345678")

    def test_hosts_retry_only_busy(self):
        for module in (roundie_package, roundie_elf, roundie_face):
            with self.subTest(module=module.__name__), patch("roundie_time.read_serial_reply", return_value=b""), \
                    patch.object(module, "read_serial_reply", return_value=b"", create=True), \
                    patch("roundie_time.write_command"), patch.object(module, "write_command", create=True), \
                    patch("roundie_time.time.sleep"):
                busy = DeviceBusy("busy")
                with patch.object(module, "parse_reply", side_effect=[busy, {"ok": 1}]) as parse:
                    self.assertEqual(module.request(None, "STATUS"), {"ok": 1})
                    self.assertEqual(parse.call_count, 2)
                with patch.object(module, "parse_reply", side_effect=[busy] * 5) as parse, \
                        self.assertRaises(DeviceBusy):
                    module.request(None, "STATUS")
                self.assertEqual(parse.call_count, BUSY_ATTEMPTS)
                with patch.object(module, "parse_reply", side_effect=[DeviceError("259"), {"ok": 1}]) as parse, \
                        self.assertRaises(DeviceError):
                    module.request(None, "INSTALL")
                self.assertEqual(parse.call_count, 1)

    def test_precondition_hint_only_for_259(self):
        for module in (roundie_package, roundie_elf, roundie_face):
            with self.subTest(module=module.__name__):
                self.assertIn("precondition, not retried", module.precondition_hint(259))
                self.assertEqual(module.precondition_hint(258), "")
                self.assertEqual(module.precondition_hint(BUSY_CODE), "")


class StorageVfsRuleTests(unittest.TestCase):
    """tools/ci/check_storage_vfs.py: no direct VFS calls outside roundie_storage."""

    def test_repository_is_clean(self):
        self.assertEqual(check_storage_vfs.violations(ROOT), [])

    def scan(self, relative, text):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / relative
            path.parent.mkdir(parents=True)
            path.write_text(text)
            return check_storage_vfs.violations(temporary)

    def test_every_direct_call_is_found(self):
        calls = ["open(p, 0)", "stat(p, &s)", "opendir(p)", "readdir(d)", "rename(a, b)", "unlink(p)",
                 "read(f, b, 1)", "write(f, b, 1)", "fopen(p, \"r\")", "mkdir(p, 0)", "fsync(f)"]
        text = "".join(f"void f{i}(void) {{ {call}; }}\n" for i, call in enumerate(calls))
        found = self.scan("main/bad.c", text)
        self.assertEqual(len(found), len(calls), found)
        self.assertEqual(found[0], "main/bad.c:1: open()")
        self.assertTrue(self.scan("components/roundie_x/y.c", "int g(void) { return unlink(\"/storage/a\"); }\n"))

    def test_helpers_comments_strings_and_markers_pass(self):
        text = ("int a(void) { return roundie_io_unlink(p); }\n"
                "/* unlink(p) in a comment */ int b(void) { return ops->open(p) + s.read(1); }\n"
                "const char *c = \"rename(a, b)\"; // stat(p)\n"
                "int d(void) { return open(\"/dev/x\", 0); } /* vfs-ok: /dev/x */\n")
        self.assertEqual(self.scan("main/good.c", text), [])
        self.assertEqual(self.scan("components/roundie_storage/z.c", "int e(void) { return unlink(p); }\n"), [])
        self.assertEqual(self.scan("main/storage_usb.c", "int e(void) { return unlink(p); }\n"), [])


if __name__ == "__main__":
    unittest.main()
