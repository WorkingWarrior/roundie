import os
from pathlib import Path
import sys
import tempfile
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "hardware_validation"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import e0_board  # noqa: E402
import run_e0  # noqa: E402
from run_m8 import Blocked  # noqa: E402
from roundie_time import DeviceError  # noqa: E402

UNIT = "usb-Espressif_USB_JTAG_serial_debug_unit_{}-if00"
A = "28:84:85:87:98:7C"
B = "AA:BB:CC:DD:EE:01"


class Unit:
    """Firmware on one port: a board name from RG1 BATTERY, or only RT1."""

    def __init__(self, name=None, rtc="NO_RTC"):
        self.name = name
        self.rtc = rtc

    def call(self, protocol, operation="STATUS", timeout=None):
        if protocol == "gauge":
            if self.name is None:
                raise DeviceError("invalid gauge command")
            return {"board": self.name}
        if protocol == "time":
            return {"state": self.rtc}
        raise AssertionError(protocol)


class FindBoardTest(unittest.TestCase):
    def by_id(self, directory, *serials):
        path = Path(directory)
        for serial in serials:
            (path / UNIT.format(serial)).symlink_to("/dev/null")
        (path / "usb-Valve_Software_Steam_Controller-if00").symlink_to("/dev/null")
        return path

    def test_single_unit_needs_no_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            by_id = self.by_id(directory, A)
            path, serial = e0_board.find_board("1.75c", by_id=by_id, device_factory=None)
            self.assertEqual(serial, A)
            self.assertTrue(path.endswith(UNIT.format(A)))

    def test_picks_the_unit_reporting_the_board(self):
        units = {A: Unit(run_e0.BOARD_NAMES["1.85b"]), B: Unit(run_e0.BOARD_NAMES["1.75c"])}
        with tempfile.TemporaryDirectory() as directory:
            by_id = self.by_id(directory, A, B)

            def factory(path, monitor):
                return units[next(s for s in units if UNIT.format(s) in path)]
            self.assertEqual(e0_board.find_board("1.75c", by_id=by_id, device_factory=factory,
                                                 monitor=types.SimpleNamespace())[1], B)
            self.assertEqual(e0_board.find_board("1.85b", by_id=by_id, device_factory=factory,
                                                 monitor=types.SimpleNamespace())[1], A)

    def test_old_firmware_falls_back_to_the_rtc_capability(self):
        units = {A: Unit(None, "NOT_FOUND"), B: Unit(None, "NO_RTC")}
        with tempfile.TemporaryDirectory() as directory:
            by_id = self.by_id(directory, A, B)

            def factory(path, monitor):
                return units[next(s for s in units if UNIT.format(s) in path)]
            self.assertEqual(e0_board.find_board("1.75c", by_id=by_id, device_factory=factory,
                                                 monitor=types.SimpleNamespace())[1], B)

    def test_ambiguous_or_missing_is_blocked(self):
        units = {A: Unit(run_e0.BOARD_NAMES["1.85b"]), B: Unit(run_e0.BOARD_NAMES["1.85b"])}
        with tempfile.TemporaryDirectory() as directory:
            by_id = self.by_id(directory, A, B)

            def factory(path, monitor):
                return units[next(s for s in units if UNIT.format(s) in path)]
            with self.assertRaises(Blocked):
                e0_board.find_board("1.75c", by_id=by_id, device_factory=factory, monitor=types.SimpleNamespace())
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(Blocked):
                e0_board.find_board("1.75c", by_id=Path(directory), device_factory=None)


class CommandTest(unittest.TestCase):
    def test_build_sources_idf_only_without_idf_py(self):
        self.assertEqual(e0_board.build_command("1.75c", which=lambda name: "/x/idf.py"),
                         ["bash", "-c", "tools/ci/build_firmware.sh waveshare_s3_touch_amoled_1_75c rep"])
        command = e0_board.build_command("1.75c", Path("/idf/export.sh"), which=lambda name: None)
        self.assertIn('source "/idf/export.sh"', command[2])

    def test_runners_share_the_unit_and_install_once(self):
        commands = dict(e0_board.runner_commands("1.75c", B, Path("/o"), Path("/i.bin"), Path("/i.elf"), "py"))
        self.assertIn("--ota", commands["base"])
        self.assertIn("--light-sleep", commands["base"])
        for name in ("followup", "stall"):
            self.assertNotIn("--ota", commands[name])
            self.assertEqual(commands[name][commands[name].index("--serial") + 1], B)

    def test_main_stops_after_a_failed_base_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image, elf = e0_board.image_paths("1.75c", root)
            image.parent.mkdir(parents=True)
            image.write_bytes(b"i")
            elf.write_bytes(b"e")
            by_id = root / "by-id"
            by_id.mkdir()
            (by_id / UNIT.format(B)).symlink_to(os.devnull)
            calls = []

            def run(command, cwd):
                calls.append(command)
                return types.SimpleNamespace(returncode=1)
            original = e0_board.image_paths
            e0_board.image_paths = lambda board: original(board, root)
            try:
                code = e0_board.main(["--board", "1.75c", "--no-build", "--out", str(root / "out")], run, by_id)
            finally:
                e0_board.image_paths = original
            self.assertEqual(code, 1)
            self.assertEqual(len(calls), 1)
            self.assertIn("run_e0.py", calls[0][1])


if __name__ == "__main__":
    unittest.main()
