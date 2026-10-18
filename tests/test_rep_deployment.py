"""Offline guards for REP development updates; no serial connection."""

import contextlib
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import flash_rep
import flash_bringup


class RepDeploymentTest(unittest.TestCase):
    def setUp(self):
        self.table = (ROOT / "build-rep/partition_table/partition-table.bin").read_bytes()
        self.boot = (ROOT / "build-rep/bootloader/bootloader.bin").read_bytes()
        self.prefix = bytearray(b"\xff" * 0x10000)
        self.prefix[:len(self.boot)] = self.boot
        self.prefix[0x8000:0x9000] = self.table.ljust(0x1000, b"\xff")
        sequence = struct.pack("<I", 1)
        self.prefix[0xd000:0xd020] = sequence + b"\xff" * 20 + struct.pack(
            "<II", 2, zlib.crc32(sequence, 0xffffffff))

    def test_rep_initial_selection_and_preserved_nvs(self):
        self.prefix[0x9000:0xd000] = bytes(0x4000)
        flash_rep.check_device(self.prefix, self.table, self.boot)

    def test_changed_layout_bootloader_and_ota_fail_closed(self):
        for offset in (0, 0x8000, 0xd000, 0xd018, 0xd01c, 0xe000):
            with self.subTest(offset=offset):
                bad = bytearray(self.prefix)
                bad[offset] ^= 1
                with self.assertRaises(ValueError):
                    flash_rep.check_device(bad, self.table, self.boot)
        with self.assertRaisesRegex(ValueError, "short"):
            flash_rep.check_device(self.prefix[:-1], self.table, self.boot)

    def test_legacy_guard_rejects_physical_rep_table(self):
        backup = flash_bringup.BACKUP.read_bytes()
        boot = (ROOT / "build/bootloader/bootloader.bin").read_bytes()
        live = bytearray(boot)
        # Stop on table equality, before bootloader or application eligibility.
        with self.assertRaisesRegex(ValueError, "live partition table differs"):
            flash_bringup.preflight(backup, self.prefix[0x8000:0x9000],
                                   b"\xff" * 0x2000, live, boot, b"\xe9", {})

    def test_invalid_rep_build_stops_before_serial_import(self):
        with mock.patch.object(sys, "argv", ["flash_rep", "--port", "HOST_ONLY", "--write"]), \
             mock.patch.object(flash_rep, "validate_build", side_effect=ValueError("profile")), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "profile"):
                flash_rep.main()


if __name__ == "__main__":
    unittest.main()
