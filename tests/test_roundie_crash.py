"""RS1 CRASH host symbolizer: record parsing, address mapping and ELF lookup."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_crash as tool


def elf32(symbols):
    """A minimal little-endian ELF32 file: null, .strtab and .symtab sections."""
    names = b"\0"
    entries = b"\0" * 16
    for name, value, size, kind in symbols:
        entries += struct.pack("<IIIBBH", len(names), value, size, 0x10 | kind, 0, 1)
        names += name.encode() + b"\0"
    strtab_offset = 52
    symtab_offset = strtab_offset + len(names)
    shoff = symtab_offset + len(entries)
    header = (b"\x7fELF\x01\x01\x01" + b"\0" * 9 +
              struct.pack("<HHIIIIIHHHHHH", 1, 94, 1, 0, 0, shoff, 0, 52, 0, 0, 40, 3, 1))
    sections = b"\0" * 40
    sections += struct.pack("<IIIIIIIIII", 0, 3, 0, 0, strtab_offset, len(names), 0, 0, 1, 0)
    sections += struct.pack("<IIIIIIIIII", 0, 2, 0, 0, symtab_offset, len(entries), 1, 1, 4, 16)
    return header + names + entries + sections


STRIPPED = b"stripped application image"
RECORD = {
    "valid": 1, "loaded": 1, "fault": 1, "instance": 7,
    "digest": hashlib.sha256(STRIPPED).hexdigest(),
    "text_exec": 0x42010000,
    "sections": [[0x0, 0x3c010000, 0x200], [0x400, 0x3fc90000, 0x40], [0x300, 0x3fc90100, 0x20],
                 [0x200, 0x3c010200, 0x100], [0, 0, 0]],
    "pc": 0x42010024, "cause": 29, "vaddr": 0x3fc90010, "sp": 0x3fcb0000, "exception": 0, "core": 0,
    "frames": [0x42010024, 0x42010104, 0x40381234],
}
APPLICATION = [("app_start", 0x0, 0x100, 2), ("fault_store", 0x100, 0x40, 2), ("counter", 0x410, 4, 1)]
FIRMWARE = [("rep_executor_step", 0x40381200, 0x80, 2)]


class SymbolTests(unittest.TestCase):
    def test_symbols_and_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "a.elf"
            path.write_bytes(elf32(APPLICATION))
            table = tool.symbols(path)
        self.assertEqual(tool.name_of(table, 0x0), "app_start")
        self.assertEqual(tool.name_of(table, 0x24), "app_start+0x24")
        self.assertEqual(tool.name_of(table, 0x13f), "fault_store+0x3f")
        self.assertIsNone(tool.name_of(table, 0x140))
        self.assertEqual(tool.name_of(table, 0x410), "counter")

    def test_rejects_non_elf32(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "x.elf"
            path.write_bytes(b"\x7fELF\x02\x01" + b"\0" * 64)
            with self.assertRaises(ValueError):
                tool.symbols(path)


class LocateTests(unittest.TestCase):
    def test_both_text_buses_data_and_end_bound(self):
        self.assertEqual(tool.locate(RECORD, 0x42010024), (0, 0x24))
        self.assertEqual(tool.locate(RECORD, 0x3c010024), (0, 0x24))
        self.assertEqual(tool.locate(RECORD, 0x3fc90010), (1, 0x410))
        self.assertEqual(tool.locate(RECORD, 0x3c010200), (3, 0x200))
        self.assertIsNone(tool.locate(RECORD, 0x42010000 + 0x200))
        self.assertIsNone(tool.locate(RECORD, 0x40381234))
        self.assertIsNone(tool.locate(RECORD, 0))


class SymbolizeTests(unittest.TestCase):
    def directory(self, debug=True):
        directory = tempfile.TemporaryDirectory()
        root = Path(directory.name)
        (root / "other.app.elf").write_bytes(b"different image")
        (root / "fault.app.elf").write_bytes(STRIPPED)
        if debug:
            (root / "fault.app.debug.elf").write_bytes(elf32(APPLICATION))
        (root / "firmware.elf").write_bytes(elf32(FIRMWARE))
        return directory

    def test_matching_digest_names_application_and_firmware_frames(self):
        with self.directory() as name:
            text = "\n".join(tool.symbolize(RECORD, [name], Path(name) / "firmware.elf"))
        self.assertIn("cause=29 (StoreProhibited)", text)
        self.assertIn("pc 0x42010024  application text 0x24 app_start+0x24", text)
        self.assertIn("#1 0x42010104  application text 0x104 fault_store+0x4", text)
        self.assertIn("#2 0x40381234  firmware rep_executor_step+0x34", text)
        self.assertIn("vaddr 0x3fc90010  application bss 0x410 counter", text)
        self.assertIn("fault.app.elf; symbols from fault.app.debug.elf", text)

    def test_unknown_digest_or_missing_debug_maps_without_names(self):
        record = dict(RECORD, digest="0" * 64)
        with self.directory() as name:
            text = "\n".join(tool.symbolize(record, [name]))
        self.assertIn("no given *.app.elf has this digest", text)
        self.assertIn("pc 0x42010024  application text 0x24\n", text + "\n")
        with self.directory(debug=False) as name:
            text = "\n".join(tool.symbolize(RECORD, [name]))
        self.assertIn(".app.debug.elf is missing", text)

    def test_firmware_fault_and_empty_records(self):
        record = dict(RECORD, loaded=0)
        text = "\n".join(tool.symbolize(record))
        self.assertIn("firmware fault", text)
        self.assertIn("pc 0x42010024  firmware", text)
        self.assertEqual(tool.symbolize({"valid": 0}), ["No valid crash record (none since power-on, or a torn record)."])
        self.assertEqual(tool.symbolize(dict(RECORD, fault=0)), ["No panic recorded since the last application load."])


class ParseTests(unittest.TestCase):
    def reply(self, record, code=0):
        return f"!RS1 0123abcd {code} {json.dumps({'crash': record})}\n".encode()

    def test_parse_valid_and_empty(self):
        self.assertEqual(tool.parse(self.reply(RECORD)), RECORD)
        self.assertEqual(tool.parse(self.reply({"valid": 0})), {"valid": 0})

    def test_rejects_malformed_records(self):
        for change in ({"valid": 2}, {"pc": -1}, {"pc": 1 << 32}, {"digest": "zz"}, {"frames": [0] * 9},
                       {"sections": RECORD["sections"][:4]}, {"core": "0"}):
            with self.subTest(change=change):
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    tool.parse(self.reply(dict(RECORD, **change)))
        with self.assertRaises(ValueError):
            tool.parse(b"garbage\n")

    def test_m9_1_origin_fields(self):
        record = dict(RECORD, frame=0x3c21f000, phase="callback", trigger=1, monitor_pc=0x42010030,
                      stack=[0x3c200000, 0x3c210000, 0x3c21a000, 0x3c220000, 1], origin="app_stack_overflow")
        self.assertEqual(tool.parse(self.reply(record)), record)
        lines = tool.symbolize(record)
        self.assertIn("Origin: app_stack_overflow - application overflowed the executor stack", lines[1])
        self.assertIn("trigger=executor stack monitor", lines[2])
        self.assertIn("red zone 0x3c210000-0x3c21a000", lines[3])
        self.assertTrue(any(line.strip().startswith("sp< 0x42010030  application text 0x30") for line in lines))
        unread = dict(record, frame=0, origin="core_while_app_active", stack=[0, 0, 0, 0, 0], monitor_pc=0)
        lines = tool.symbolize(unread)
        self.assertIn("not attributed", lines[1])
        self.assertIn("exception frame not read", "\n".join(lines))
        for change in ({"phase": "running"}, {"origin": "app"}, {"stack": [0, 0, 0, 0]}, {"trigger": -1},
                       {"monitor_pc": "0"}):
            with self.subTest(change=change):
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    tool.parse(self.reply(dict(record, **change)))
        partial = dict(record)
        del partial["stack"]
        with self.assertRaises(KeyError):
            tool.parse(self.reply(partial))

    def test_older_firmware_is_unsupported(self):
        with self.assertRaises(tool.Unsupported):
            tool.parse(b'!RS1 0123abcd -1 {"error":"unknown operation"}\n')

    def test_main_report_file(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "crash.json"
            report.write_text(json.dumps({"crash": RECORD}))
            self.assertEqual(tool.main(["--report", str(report), "--json"]), 0)
            report.write_text(json.dumps({"crash": dict(RECORD, pc="x")}))
            self.assertEqual(tool.main(["--report", str(report)]), 2)


if __name__ == "__main__":
    unittest.main()
