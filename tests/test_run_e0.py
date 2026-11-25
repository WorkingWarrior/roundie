import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "hardware_validation"))
import run_e0  # noqa: E402
from run_e0 import BLOCKED, FAIL, PASS, PENDING, DeviceError, TransportError  # noqa: E402


def reference_pattern(kib, seed=0x9E3779B9):
    """Independent spelling of the xorshift32 pattern of main/storage_bench.c."""
    words = []
    x = seed
    for _ in range(kib * 256):
        x = (x ^ (x << 13)) & 0xFFFFFFFF
        x = x ^ (x >> 17)
        x = (x ^ (x << 5)) & 0xFFFFFFFF
        words.append(x)
    return struct.pack(f"<{len(words)}I", *words)


class FakePort:
    """Answers one written RP1 command with a canned payload."""

    def __init__(self, code, payload):
        self.code = code
        self.payload = payload
        self.out = b""
        self.written = b""

    def write(self, data):
        self.written += data
        identity = data.split()[1].decode()
        self.out = f"log line\n!RP1 {identity} {self.code} {json.dumps(self.payload)}\n".encode()
        return len(data)

    def read(self, size=1):
        byte, self.out = self.out[:size], self.out[size:]
        return byte


def fsbench_payload(operation="READ", kib=64, digest=""):
    return {"format": 1, "fsbench": operation, "kib": kib, "bytes": kib * 1024, "write_us": 0,
            "read_us": 1000, "sha_us": 200, "total_us": 1300, "chunk": 4096, "chunk_max_us": 90,
            "capacity": [1, 9240576, 163840], "sha256": digest}


class PatternTest(unittest.TestCase):
    def test_matches_independent_reference(self):
        for kib in (64, 512):
            self.assertEqual(run_e0.fs_pattern_sha256(kib), hashlib.sha256(reference_pattern(kib)).hexdigest())

    def test_sizes_differ(self):
        digests = {run_e0.fs_pattern_sha256(kib) for kib in run_e0.FS_SIZES_KIB}
        self.assertEqual(len(digests), 3)


class FsbenchRequestTest(unittest.TestCase):
    def test_parses_read(self):
        port = FakePort(0, fsbench_payload(digest="ab" * 32))
        result = run_e0.fsbench_request(port, "READ 64", timeout=1)
        self.assertEqual(result["code"], 0)
        self.assertEqual(result["sha256"], "ab" * 32)
        self.assertIn(b"FSBENCH READ 64", port.written)

    def test_keeps_refusal_code(self):
        port = FakePort(259, fsbench_payload("PREP", 1024))
        self.assertEqual(run_e0.fsbench_request(port, "PREP 1024", timeout=1)["code"], 259)

    def test_rejects_other_sizes_and_operations(self):
        for operation in ("READ 128", "WRITE 64", "READ 64 x", "DEL"):
            with self.assertRaises(DeviceError):
                run_e0.fsbench_request(FakePort(0, {}), operation, timeout=1)

    def test_rejects_mismatched_reply(self):
        port = FakePort(0, fsbench_payload("PREP"))
        with self.assertRaises(TransportError):
            run_e0.fsbench_request(port, "READ 64", timeout=1)


class SemanticInputTest(unittest.TestCase):
    def ui(self, **values):
        status = {"generation": 7, "widgets": 5, "last_widget": 25}
        status.update(values)
        return status

    def test_roles_follow_creation_order(self):
        roles = run_e0.stopwatch_roles(self.ui(), 7)
        self.assertEqual(roles, {"view": 21, "dial": 22, "time": 23, "laps": 24, "lap": 25})
        self.assertEqual(run_e0.semantic_operation("PRESS", 7, roles, "dial"), "PRESS 7 22")
        self.assertEqual(run_e0.semantic_operation("VALUE", 7, roles, "time", -3), "VALUE 7 23 -3")

    def test_refuses_foreign_or_incomplete_ui(self):
        with self.assertRaises(DeviceError):
            run_e0.stopwatch_roles(self.ui(generation=6), 7)
        with self.assertRaises(DeviceError):
            run_e0.stopwatch_roles(self.ui(widgets=4), 7)
        with self.assertRaises(DeviceError):
            run_e0.stopwatch_roles(self.ui(last_widget=3), 7)

    def test_refuses_unknown_role_and_kind(self):
        roles = run_e0.stopwatch_roles(self.ui(), 7)
        with self.assertRaises(DeviceError):
            run_e0.semantic_operation("PRESS", 7, roles, "start")
        with self.assertRaises(DeviceError):
            run_e0.semantic_operation("VALUE", 7, roles, "dial")
        with self.assertRaises(DeviceError):
            run_e0.semantic_operation("HOLD", 7, roles, "dial")


def trace(sequence, flushes=1, dma=100):
    return {"sequence": sequence, "flushes": flushes, "dma_us": dma, "render_us": 2 * dma,
            "submit_us": 10, "wait_us": 5, "handler_us": 3 * dma, "scale_us": 0}


class GuiWindowTest(unittest.TestCase):
    def test_statistics_over_flushing_iterations(self):
        window = run_e0.gui_window([trace(9), trace(10, dma=100), trace(11, flushes=0, dma=999), trace(12, dma=300)], 9)
        self.assertEqual(window["iterations"], 3)
        self.assertEqual(window["flushing"], 2)
        self.assertFalse(window["truncated"])
        self.assertEqual(window["dma_us"], {"max": 300, "mean": 200.0})

    def test_marks_lost_iterations(self):
        self.assertTrue(run_e0.gui_window([trace(50)], 10)["truncated"])
        self.assertEqual(run_e0.gui_window([], 10), {"iterations": 0, "flushing": 0, "truncated": False})


HEALTH = {"internal_free": 100, "internal_min": 90, "internal_largest": 80, "psram_free": 1000,
          "psram_min": 900, "psram_largest": 800, "alloc_failed": 0, "tasks": 20,
          "stacks": {"main": 3000, "system_worker": 6000}, "executor_guard": {}}


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds


class SleepDevice:
    """A board that drops USB while it sleeps, then answers again."""

    def __init__(self, clock, reset=False):
        self.clock = clock
        self.path = "fake"
        self.sleep_until = None
        self.sleeps = 0
        self.uptime = 1_000_000
        self.reset = reset

    def call(self, protocol, operation="STATUS", timeout=None):
        if self.sleep_until is not None and self.clock.t < self.sleep_until:
            raise TransportError("port gone")
        if self.sleep_until is not None:
            self.sleep_until = None
            self.sleeps += 1
            self.uptime = 10 if self.reset else self.uptime + 25_000_000
        if protocol == "power":
            if operation.startswith("SLEEP"):
                self.sleep_until = self.clock.t + 1.0 + int(operation.split()[1])
            return {"phase": "idle", "state": "active", "sleeps": self.sleeps, "refusal": "none"}
        if protocol == "safe":
            self.uptime += 1000
            return dict(HEALTH, uptime_us=self.uptime)
        if protocol == "launcher":
            return {"state": "HOME", "generation": 0, "request": 0, "revision": 1, "selected": ""}
        if protocol == "perf":
            return {"summary": {}, "latest": 0, "oldest": 0}
        if protocol == "package":
            return {"capacity": [1, 9000, 1000]}
        if protocol == "ui":
            return {"generation": 0, "widgets": 0, "outstanding": 0, "lvgl_bytes": 0, "lvgl_peak": 0}
        raise AssertionError(protocol)


class LightSleepTest(unittest.TestCase):
    def run_sleep(self, reset=False, allowed=True):
        clock = FakeClock()
        device = SleepDevice(clock, reset)
        monitor = types.SimpleNamespace(faults=[], notices=[])
        args = types.SimpleNamespace(light_sleep=allowed, sleep_seconds=5, settle=0)
        with tempfile.TemporaryDirectory() as out:
            runner = run_e0.Runner(args, device, monitor, out, clock.sleep, clock.now)
            return runner.run_scenario("light_sleep"), runner

    def test_tolerates_usb_loss_and_reads_state_after_return(self):
        entry, runner = self.run_sleep()
        self.assertEqual(entry["status"], PASS, entry["details"])
        details = entry["details"]
        self.assertIsNotNone(details["usb_lost_after_s"])
        self.assertGreaterEqual(details["usb_back_after_s"], 5)
        self.assertEqual(details["wake_time"], PENDING)
        self.assertIn("after_light_sleep", runner.snapshots)

    def test_reset_instead_of_wake_fails(self):
        entry, runner = self.run_sleep(reset=True)
        self.assertEqual(entry["status"], FAIL)
        self.assertTrue(runner.report["monitoring"]["unexpected_resets"])

    def test_blocked_without_permission(self):
        entry, _ = self.run_sleep(allowed=False)
        self.assertEqual(entry["status"], BLOCKED)


class GateTest(unittest.TestCase):
    def report(self, steps, snapshots, power=True):
        return {"steps": [{"name": n, "status": s} for n, s in steps.items()],
                "snapshots": snapshots, "context": {"power": {} if power else None},
                "pending": [{"needs": "current", "status": PENDING}],
                "monitoring": {"unexpected_resets": []}}

    def test_complete_run(self):
        snap = {"heap": {}, "stacks": {}, "gui_window": {}, "littlefs": {}}
        snapshots = {name: snap for name in ("home", "stopwatch", "full_refresh", "after_close")}
        steps = dict.fromkeys(("home", "stopwatch", "after_close", "full_refresh", "littlefs", "light_sleep"), PASS)
        report = self.report(steps, snapshots)
        items = dict(run_e0.gate(report))
        self.assertEqual(items["Heap internal/PSRAM current, minimum, largest block"], PASS)
        self.assertEqual(items["current"], PENDING)
        self.assertEqual(run_e0.outcome(report), PASS)

    def test_missing_scenarios(self):
        report = self.report({"home": PASS, "light_sleep": BLOCKED}, {"home": {"heap": {}}})
        items = dict(run_e0.gate(report))
        self.assertEqual(items["Heap internal/PSRAM current, minimum, largest block"], FAIL)
        self.assertEqual(items["GUI/DMA times"], BLOCKED)
        self.assertEqual(run_e0.outcome(report), BLOCKED)
        report["steps"].append({"name": "littlefs", "status": FAIL})
        self.assertEqual(run_e0.outcome(report), FAIL)


class DiscoveryTest(unittest.TestCase):
    def test_missing_board_is_blocked(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "out"
            code = run_e0.main(["--board", "1.75c", "--serial", "AA:BB:CC:DD:EE:FF", "--out", str(out),
                                "--commit", "test"], by_id=Path(directory) / "none")
            self.assertEqual(code, 2)
            self.assertEqual(json.loads((out / "report.json").read_text())["outcome"], BLOCKED)


if __name__ == "__main__":
    unittest.main()
