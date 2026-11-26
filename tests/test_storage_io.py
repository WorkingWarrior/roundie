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
sys.path.insert(0, str(ROOT / "tools/ci"))
import check_storage_vfs  # noqa: E402
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

    def test_bounded_io_fake_vfs_and_guard(self):
        output = compile_and_run("test_storage_guard.c", STORAGE / "bounded_io.c", STORAGE / "claim.c",
                                 defines=("-DROUNDIE_IO_READ=fake_read", "-DROUNDIE_IO_WRITE=fake_write"))
        self.assertIn("storage guard: PASS", output)

    def test_hosts_recognize_busy(self):
        for module, prefix in ((roundie_package, "RP1"), (roundie_elf, "RE1"), (roundie_face, "RF1")):
            with self.subTest(prefix=prefix):
                line = f'\n!{prefix} 12345678 259 {{"format":1,"error":"busy"}}\n'.encode()[1:]
                with self.assertRaises(DeviceBusy):
                    module.parse_reply(line, "12345678")


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
