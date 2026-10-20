#!/usr/bin/env python3
"""Expand the real pinned TinyUSB macros and inspect their resulting bytes.

Host structure evidence only: this never opens USB or proves enumeration.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
TINYUSB = ROOT / "managed_components/espressif__esp_tinyusb/tinyusb/src"


class Descriptors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (TINYUSB / "tusb.h").is_file():
            raise RuntimeError("Resolve the pinned probe dependency with idf.py reconfigure first")
        with tempfile.TemporaryDirectory(prefix="roundie-usb-descriptors-") as directory:
            library = Path(directory) / "descriptors.so"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                "-I" + str(ROOT / "host_config"), "-I" + str(TINYUSB),
                str(ROOT / "main/descriptors.c"), "-o", str(library),
            ], check=True)
            module = ctypes.CDLL(str(library))
            prefix = bytes((ctypes.c_uint8 * 9).in_dll(module, "roundie_probe_configuration"))
            size = struct.unpack_from("<H", prefix, 2)[0]
            cls.raw = bytes((ctypes.c_uint8 * size).in_dll(module, "roundie_probe_configuration"))
        cls.records = []
        offset = 0
        while offset < len(cls.raw):
            size = cls.raw[offset]
            if size < 2 or offset + size > len(cls.raw):
                raise AssertionError("invalid descriptor boundary")
            cls.records.append(cls.raw[offset:offset + size])
            offset += size

    def test_configuration(self):
        self.assertEqual(len(self.raw), 105)
        self.assertEqual(self.raw[:9], bytes([9, 2, 105, 0, 3, 1, 0, 0xC0, 50]))

    def test_interfaces(self):
        interfaces = [d for d in self.records if d[1] == 4]
        self.assertEqual([(d[2], d[3], d[4], d[5], d[6], d[7]) for d in interfaces],
                         [(0, 0, 1, 2, 2, 0), (1, 0, 2, 10, 0, 0), (2, 0, 3, 6, 1, 1)])

    def test_cdc_association(self):
        self.assertEqual([d for d in self.records if d[1] == 11],
                         [bytes([8, 11, 0, 2, 2, 2, 0, 0])])
        union = [d for d in self.records if d[1:3] == bytes([0x24, 6])]
        self.assertEqual(union, [bytes([5, 0x24, 6, 0, 1])])

    def test_endpoints(self):
        endpoints = [d for d in self.records if d[1] == 5]
        self.assertEqual([d[2] for d in endpoints], [0x81, 0x02, 0x82, 0x84, 0x03, 0x83])
        self.assertEqual(len({d[2] for d in endpoints}), 6)
        self.assertEqual(sum(bool(d[2] & 0x80) for d in endpoints) + 1, 5)
        self.assertEqual(sum(not d[2] & 0x80 for d in endpoints) + 1, 3)
        self.assertTrue(all(0 < d[2] & 0x0F <= 4 for d in endpoints))

    def test_full_speed_packet_sizes(self):
        endpoints = [d for d in self.records if d[1] == 5]
        self.assertEqual([(d[3], int.from_bytes(d[4:6], "little")) for d in endpoints],
                         [(3, 8), (2, 64), (2, 64), (3, 64), (2, 64), (2, 64)])


if __name__ == "__main__":
    unittest.main()
