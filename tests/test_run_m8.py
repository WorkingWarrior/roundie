import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "hardware_validation"))
import run_m8  # noqa: E402
from run_m8 import TransportError, DeviceError  # noqa: E402

SERIAL = "28:84:85:87:98:7C"
OTHER_SERIAL = "AA:BB:CC:DD:EE:FF"
ELF_NOW = "11" * 32
ELF_OLD = "22" * 32


def entry_name(serial):
    return f"usb-Espressif_USB_JTAG_serial_debug_unit_{serial}-if00"


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds


class FakeDevice:
    """In-memory board with the interface of run_m8.Device."""

    def __init__(self, clock, path="fake", hook=None):
        self.clock = clock
        self.path = path
        self.hook = hook
        self.calls = []
        self.session_calls = 0
        self.down_until = -1.0
        self.boot_t = -100.0
        self.restart_resets = True
        self.reset_on_clock_call = None
        self.fail_select = False
        self.exit_works = True
        self.ota = dict(phase="IDLE", running="ota_0", boot="ota_0", other="ota_1", ota_state="VALID",
                        checked=1, passed=1, rollback=1, other_image=1, other_state="VALID",
                        other_elf=ELF_OLD, elf=ELF_NOW, version="0.8.0", last_error="", digest="")
        self.elf = dict(allowed=1, busy=0, state=0, instance=0, abi=2, loader="1.3.5", error="",
                        over_budget=0, resources=[0, 0, 0, 0], users=0, value=0, ui_events=0,
                        rejected=0, after=[1, 1, 1, 1, 1], load_us=0, unload_us=0, dropped=0,
                        app_result=0, executed=0, control=[0, 0, 0])
        self.ui = dict(generation=0, view=0, button=0, widgets=0, pending=0, max_depth=0, submitted=0,
                       applied=0, failed=0, rejected=0, canceled=0, input_accepted=0, input_dropped=0,
                       input_rejected=0, physical=0, injected=0, delivered=0, latency_max_us=0,
                       enqueue_max_us=0, create_max_us=0, update_max_us=0, teardown_us=0,
                       input_latency_max_us=0, acquired=0, released=0, outstanding=0, lvgl_bytes=0,
                       lvgl_peak=0, flushes=0, flush_errors=0, last_widget=0, text_hash=0,
                       foreground=0, admitting=0, pool_before=0, pool_free=0, realized_objects=0,
                       values=0, error=0)
        self.nav = dict(state="ACTIVE", generation=1, revision=1, selected=run_m8.HOME, count=3)
        self.power = dict(state="active", phase="idle", refusal=0, sleeps=0, last_sleep_ms=0, screen_offs=0)
        self.tz = "CET-1CEST,M3.5.0,M10.5.0/3"
        self.safe = dict(safe=0, reason="none", latch=0)
        self.monitor = None

    def count(self, protocol):
        return sum(1 for p, _ in self.calls if p == protocol)

    def operations(self, protocol):
        return [o for p, o in self.calls if p == protocol]

    def session(self, function):
        self.session_calls += 1
        raise AssertionError("session() is not modelled; the test should not reach it")

    def wall(self):
        return int(time.time() + self.clock.t)

    def call(self, protocol, operation="STATUS", timeout=None):
        self.calls.append((protocol, operation))
        if self.hook is not None:
            self.hook(self, protocol, operation)
        if self.clock.t < self.down_until:
            raise TransportError("device is down")
        if protocol == "clock":
            if self.reset_on_clock_call == self.count("clock"):
                self.boot_t = self.clock.t
            return {"uptime_us": int((self.clock.t - self.boot_t) * 1e6)}
        if protocol == "ota":
            if operation == "RESTART":
                self.down_until = self.clock.t + 3.0
                self.nav = dict(self.nav, state="ACTIVE", selected=run_m8.HOME)
                if self.restart_resets:
                    self.boot_t = self.down_until
            return dict(self.ota)
        if protocol == "elf":
            return dict(self.elf)
        if protocol == "ui":
            return dict(self.ui)
        if protocol == "launcher":
            return self.launcher(operation)
        if protocol == "power":
            return self.power_op(operation)
        if protocol == "time":
            wall = self.wall()
            return dict(code="OK", state=0, os=0, rtc=wall, wall=wall, error=0, round_trip=0.01, sent=wall)
        if protocol == "safe":
            return dict(self.safe)
        if protocol == "zone":
            if operation is not None:
                self.tz = operation
            return self.tz
        raise AssertionError(f"unmodelled protocol {protocol}")

    def launcher(self, operation):
        words = operation.split()
        if words[0] == "EXIT":
            if self.exit_works:
                self.nav["state"] = "LIST"
        elif words[0] == "SELECT":
            if self.fail_select:
                raise DeviceError("select refused")
            self.nav.update(state="ACTIVE", selected=words[2], generation=self.nav["generation"] + 1)
        elif words[0] == "REFRESH":
            self.nav["revision"] += 1
        return dict(self.nav)

    def power_op(self, operation):
        if operation == "SCREEN OFF":
            self.power.update(state="screen_off", screen_offs=self.power["screen_offs"] + 1)
        elif operation == "SCREEN ON":
            self.power["state"] = "active"
        return dict(self.power)


class RunnerTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.by_id = self.root / "by-id"
        self.by_id.mkdir()
        self.out = self.root / "out"
        self.clock = FakeClock()
        self.units = {}
        self.add_unit(SERIAL)

    def add_unit(self, serial):
        target = self.root / f"tty-{serial.replace(':', '')}"
        target.write_text("")
        link = self.by_id / entry_name(serial)
        link.symlink_to(target)
        self.units[serial] = link
        return link

    def device(self, hook=None):
        self.fake = FakeDevice(self.clock, hook=hook)
        return self.fake

    def run_main(self, *argv, fake=None):
        created = []

        def factory(path, monitor):
            created.append(path)
            if fake is not None:
                fake.path = path
                fake.monitor = monitor
                return fake
            raise AssertionError("no device expected")

        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = run_m8.main(["--out", str(self.out), *argv], device_factory=factory, by_id=self.by_id,
                               sleep=self.clock.sleep, now=self.clock.now)
        self.created = created
        self.stderr = stderr.getvalue()
        return code, json.loads((self.out / "report.json").read_text())

    @staticmethod
    def step(report, name):
        return next(s for s in report["steps"] if s["name"] == name)


class PreflightTests(RunnerTestCase):
    def test_good_preflight_and_core_status_pass(self):
        fake = self.device()
        code, report = self.run_main("--only", "core_status", fake=fake)
        self.assertEqual(code, 0)
        self.assertEqual(report["preflight"]["status"], "PASS")
        self.assertEqual(report["device"]["serial"], SERIAL)
        self.assertEqual(report["device"]["firmware"], "0.8.0")
        self.assertEqual(report["device"]["board_detected"], "1.85b")
        self.assertEqual(report["outcome"], "PASS")
        self.assertEqual([s["status"] for s in report["steps"]], ["PASS"])
        self.assertTrue((self.out / "report.md").is_file())
        self.assertIn("Outcome: PASS", (self.out / "report.md").read_text())
        self.assertIsNotNone(report["finished"])
        self.assertEqual(report["monitoring"]["unexpected_resets"], [])
        self.assertTrue(all(m["status"] == "PENDING" for m in report["manual"]))

    def test_unhealthy_firmware_blocks_before_any_step(self):
        fake = self.device()
        fake.ota["ota_state"] = "NEW"
        code, report = self.run_main("--only", "core_status", fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertEqual(report["preflight"]["status"], "FAIL")
        self.assertEqual(report["steps"], [])

    def test_safe_mode_blocks_before_any_step(self):
        fake = self.device()
        fake.safe = dict(safe=1, reason="core", latch=5)
        code, report = self.run_main("--only", "core_status", fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertTrue(any("Safe Mode" in p for p in report["preflight"]["problems"]))
        self.assertEqual(report["steps"], [])
        self.assertNotIn("REBOOT", fake.operations("safe"))

    def test_wrong_board_blocks(self):
        fake = self.device()
        code, report = self.run_main("--board", "1.75c", "--only", "core_status", fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertTrue(any("board is 1.85b" in p for p in report["preflight"]["problems"]))

    def test_unreachable_device_in_preflight_blocks(self):
        def dead(device, protocol, operation):
            raise TransportError("response timeout")

        code, report = self.run_main("--only", "core_status", fake=self.device(dead))
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertIn("no valid Roundie reply", report["preflight"]["problems"][0])


class DiscoveryTests(RunnerTestCase):
    def test_no_device_blocks_without_device_calls(self):
        self.units[SERIAL].unlink()
        code, report = self.run_main("--only", "core_status")
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertEqual(report["preflight"]["status"], "BLOCKED")
        self.assertEqual(self.created, [])
        self.assertIn("BLOCKED", self.stderr)
        self.assertEqual(report["steps"], [])

    def test_port_of_a_different_serial_blocks(self):
        other = self.add_unit(OTHER_SERIAL)
        fake = self.device()
        code, report = self.run_main("--port", str(other), "--serial", SERIAL, fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertIn("is not the USB Serial/JTAG unit", report["preflight"]["problems"][0])
        self.assertEqual(self.created, [])
        self.assertEqual(fake.calls, [])

    def test_two_units_without_serial_block(self):
        self.add_unit(OTHER_SERIAL)
        fake = self.device()
        code, report = self.run_main(fake=fake)
        self.assertEqual(code, 2)
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertIn("2 Espressif", report["preflight"]["problems"][0])
        self.assertEqual(fake.calls, [])

    def test_two_units_with_serial_selects_that_unit(self):
        other = self.add_unit(OTHER_SERIAL)
        fake = self.device()
        code, report = self.run_main("--serial", OTHER_SERIAL.lower(), "--only", "core_status", fake=fake)
        self.assertEqual(code, 0)
        self.assertEqual(self.created, [str(other)])
        self.assertEqual(report["device"]["serial"], OTHER_SERIAL)

    def test_port_outside_by_id_blocks(self):
        stray = self.root / "ttyACM9"
        stray.write_text("")
        code, report = self.run_main("--port", str(stray), fake=self.device())
        self.assertEqual(code, 2)
        self.assertEqual(self.created, [])


class ScenarioTests(RunnerTestCase):
    def test_communication_loss_blocks_later_steps(self):
        def dead_after_preflight(device, protocol, operation):
            if device.count("clock") >= 1 and protocol != "clock":
                raise TransportError("serial disconnected")

        fake = self.device(dead_after_preflight)
        code, report = self.run_main("--only", "core_status", "rtc", "screen", fake=fake)
        first = self.step(report, "core_status")
        self.assertEqual(first["status"], "FAIL")
        self.assertIs(first["details"]["reconnected"], False)
        self.assertIn("communication", first["details"]["error"])
        for name in ("rtc", "screen"):
            later = self.step(report, name)
            self.assertEqual(later["status"], "BLOCKED")
            self.assertIn("device unavailable", later["details"]["reason"])
        self.assertIn("skipped: device unavailable", report["restore"])
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_timeout_fails_step_and_runner_continues(self):
        fake = self.device()
        fake.exit_works = False
        code, report = self.run_main("--only", "launcher", "core_status", fake=fake)
        launcher_step = self.step(report, "launcher")
        self.assertEqual(launcher_step["status"], "FAIL")
        self.assertIn("timeout", launcher_step["details"]["error"])
        self.assertEqual(self.step(report, "core_status")["status"], "PASS")
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_launcher_scenario_passes_and_advances_generation(self):
        fake = self.device()
        code, report = self.run_main("--only", "launcher", fake=fake)
        self.assertEqual(self.step(report, "launcher")["status"], "PASS")
        self.assertEqual(report["outcome"], "PASS")
        self.assertEqual(code, 0)
        self.assertEqual(fake.nav["generation"], 2)

    def test_launcher_waits_for_home_started_after_boot(self):
        # After boot the launcher shows LIST before home starts (seen on the
        # 1.85B, M8 run 3): a busy runtime there is home loading.
        def boot(device, protocol, operation):
            if protocol == "elf" and device.count("elf") == 30:
                device.nav.update(state="ACTIVE", selected=run_m8.HOME, generation=1)
                device.elf.update(state=3)
            if protocol == "launcher" and operation.startswith("EXIT"):
                device.elf.update(state=0, busy=0)

        fake = self.device(boot)
        fake.nav.update(state="LIST", selected="", generation=0)
        fake.elf.update(state=1, busy=1)
        code, report = self.run_main("--only", "launcher", fake=fake)
        self.assertEqual(self.step(report, "launcher")["status"], "PASS", report["steps"])
        self.assertIn("EXIT 1", fake.operations("launcher"))
        self.assertEqual(code, 0)

    def test_core_status_failure_on_ui_counter(self):
        fake = self.device()
        fake.ui["failed"] = 1
        code, report = self.run_main("--only", "core_status", fake=fake)
        step = self.step(report, "core_status")
        self.assertEqual(step["status"], "FAIL")
        self.assertIn("UI error, failed command or flush error", step["details"]["problems"])
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_skip_removes_a_step(self):
        fake = self.device()
        code, report = self.run_main("--only", "core_status", "screen", "--skip", "screen", fake=fake)
        self.assertEqual([s["name"] for s in report["steps"]], ["core_status"])

    def test_missing_app_blocks_step(self):
        fake = self.device()
        code, report = self.run_main("--apps", str(self.root / "no-apps"), "--only", "elf_hello", fake=fake)
        step = self.step(report, "elf_hello")
        self.assertEqual(step["status"], "BLOCKED")
        self.assertIn("is missing", step["details"]["reason"])
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertEqual(code, 2)
        self.assertEqual(fake.session_calls, 0)

    def test_screen_scenario_passes(self):
        fake = self.device()
        code, report = self.run_main("--only", "screen", fake=fake)
        self.assertEqual(self.step(report, "screen")["status"], "PASS")
        self.assertEqual(fake.power["state"], "active")

    def test_light_sleep_blocked_without_flag(self):
        code, report = self.run_main("--only", "light_sleep", fake=self.device())
        step = self.step(report, "light_sleep")
        self.assertEqual(step["status"], "BLOCKED")
        self.assertIn("--light-sleep", step["details"]["reason"])
        self.assertEqual(code, 2)


class OtaTests(RunnerTestCase):
    def make_image(self):
        image = bytearray(512)
        image[0] = 0xe9
        image[1] = 4
        image[12:14] = (9).to_bytes(2, "little")
        image[32:36] = (0xabcd5432).to_bytes(4, "little")
        image[48:48 + 5] = b"0.9.0"
        image[80:80 + len(b"roundie_rep")] = b"roundie_rep"
        image[176:208] = bytes.fromhex("33" * 32)
        path = self.root / "roundie_rep.bin"
        path.write_bytes(bytes(image))
        return path

    def test_ota_refused_without_valid_rollback_image(self):
        fake = self.device()
        fake.ota["other_state"] = "UNDEFINED"
        image = self.make_image()
        code, report = self.run_main("--ota", str(image), "--only", "core_status", fake=fake)
        self.assertEqual(code, 1)
        self.assertEqual(report["ota"]["status"], "BLOCKED")
        self.assertIn("rollback", report["ota"]["reason"])
        self.assertEqual(report["outcome"], "BLOCKED")
        self.assertEqual(report["steps"], [])
        self.assertEqual(fake.session_calls, 0)
        ota_ops = fake.operations("ota")
        self.assertFalse([o for o in ota_ops if o.split()[0] in ("ACTIVATE", "RESTART", "BEGIN", "DATA")])
        self.assertFalse(report["preflight"]["recovery_path"])

    def test_ota_of_unrecognised_image_is_failure_not_write(self):
        fake = self.device()
        bad = self.root / "bad.bin"
        bad.write_bytes(b"\0" * 400)
        code, report = self.run_main("--ota", str(bad), fake=fake)
        self.assertEqual(code, 1)
        self.assertEqual(report["ota"]["status"], "FAIL")
        self.assertEqual(report["steps"], [])
        self.assertEqual(fake.session_calls, 0)

    def test_ota_of_running_image_writes_nothing(self):
        fake = self.device()
        image = self.make_image()
        data = bytearray(image.read_bytes())
        data[176:208] = bytes.fromhex(ELF_NOW)
        image.write_bytes(bytes(data))
        code, report = self.run_main("--ota", str(image), "--only", "core_status", fake=fake)
        self.assertEqual(report["ota"]["status"], "PASS")
        self.assertIn("nothing written", report["ota"]["note"])
        self.assertEqual(fake.session_calls, 0)
        self.assertEqual(code, 0)


class RestartTests(RunnerTestCase):
    def test_restart_reconnects_and_reset_is_expected(self):
        fake = self.device()
        code, report = self.run_main("--only", "restart", fake=fake)
        step = self.step(report, "restart")
        self.assertEqual(step["status"], "PASS", step["details"])
        self.assertEqual(report["monitoring"]["unexpected_resets"], [])
        self.assertEqual(report["outcome"], "PASS")
        self.assertEqual(code, 0)
        self.assertIn("ACTIVE", step["details"]["home"]["state"])
        self.assertTrue(any(u.get("reset") for u in report["monitoring"]["uptime"]))
        self.assertGreaterEqual(self.clock.t, 3.0)

    def test_restart_without_uptime_drop_fails(self):
        fake = self.device()
        fake.restart_resets = False
        code, report = self.run_main("--only", "restart", fake=fake)
        step = self.step(report, "restart")
        self.assertEqual(step["status"], "FAIL")
        self.assertTrue(any("no reset observed" in p for p in step["details"]["problems"]))
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_restart_disabled_is_blocked(self):
        fake = self.device()
        code, report = self.run_main("--only", "restart", "--no-restart", fake=fake)
        self.assertEqual(self.step(report, "restart")["status"], "BLOCKED")
        self.assertNotIn("RESTART", fake.operations("ota"))

    def test_device_never_returns_after_restart(self):
        fake = self.device()
        fake.restart_resets = False
        original = fake.call

        def never_back(protocol, operation="STATUS", timeout=None):
            result = original(protocol, operation, timeout)
            if protocol == "ota" and operation == "RESTART":
                fake.down_until = 10 ** 9
            return result

        fake.call = never_back
        code, report = self.run_main("--only", "restart", "core_status", fake=fake)
        self.assertEqual(self.step(report, "restart")["status"], "FAIL")
        self.assertEqual(self.step(report, "core_status")["status"], "BLOCKED")
        self.assertEqual(report["outcome"], "FAIL")


class MonitoringTests(RunnerTestCase):
    def test_unexpected_reset_between_steps(self):
        fake = self.device()
        fake.reset_on_clock_call = 3  # preflight, after step 1, after step 2
        code, report = self.run_main("--only", "core_status", "core_status", "core_status", fake=fake)
        # The step during which the reset happened fails; later steps would
        # only report its consequences, so they are blocked.
        self.assertEqual([s["status"] for s in report["steps"]], ["PASS", "FAIL", "BLOCKED"])
        self.assertIn("halted", report["steps"][1]["details"])
        self.assertTrue(any(item.startswith("skipped") for item in report["restore"]))
        resets = report["monitoring"]["unexpected_resets"]
        self.assertEqual(len(resets), 1)
        self.assertEqual(resets[0]["label"], "after core_status")
        self.assertLess(resets[0]["after_us"], resets[0]["before_us"])
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_partial_report_on_interrupt(self):
        def interrupt(device, protocol, operation):
            if (protocol, operation) == ("power", "SCREEN ON"):
                raise KeyboardInterrupt

        fake = self.device(interrupt)
        code, report = self.run_main("--only", "core_status", "screen", fake=fake)
        self.assertEqual(code, 130)
        self.assertIs(report["interrupted"], True)
        self.assertEqual(report["outcome"], "INTERRUPTED")
        self.assertEqual([s["name"] for s in report["steps"]], ["core_status"])
        self.assertIsNotNone(report["finished"])

    def test_failed_state_restore(self):
        fake = self.device()
        fake.fail_select = True
        code, report = self.run_main("--only", "launcher", fake=fake)
        self.assertTrue(any("FAILED" in item for item in report["restore"]), report["restore"])
        self.assertIs(report["restore_failed"], True)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_successful_restore_returns_home(self):
        fake = self.device()
        code, report = self.run_main("--only", "launcher", fake=fake)
        self.assertEqual(fake.nav["state"], "ACTIVE")
        self.assertEqual(fake.nav["selected"], run_m8.HOME)
        self.assertNotIn("restore_failed", report)

    def test_serial_fault_during_step_halts_later_steps(self):
        def panic(device, protocol, operation):
            if protocol == "ota" and device.count("ota") == 2:  # preflight, then core_status
                device.monitor.received(b"Guru Meditation Error: Core  0 panic'ed (IntegerDivideByZero).\n")

        fake = self.device(panic)
        code, report = self.run_main("--only", "core_status", "screen", fake=fake)
        self.assertEqual([s["status"] for s in report["steps"]], ["FAIL", "BLOCKED"])
        self.assertEqual(report["monitoring"]["faults"][0]["pattern"], "Guru Meditation")
        self.assertIs(report["restore_failed"], True)
        self.assertEqual(fake.operations("power").count("SCREEN OFF"), 0)
        self.assertEqual(report["outcome"], "FAIL")
        self.assertEqual(code, 1)

    def test_serial_fault_in_log_fails_outcome(self):
        runner_monitor = run_m8.Monitor(None, time.monotonic)
        report = {"steps": [{"status": "PASS"}], "preflight": {"status": "PASS"},
                  "monitoring": {"unexpected_resets": [], "faults": runner_monitor.faults}}
        self.assertEqual(run_m8.outcome(report), "PASS")
        runner_monitor.received(b"Guru Meditation Error: Core 0 panic'ed\n")
        self.assertEqual(run_m8.outcome(report), "FAIL")


class HelperTests(unittest.TestCase):
    def test_leak_check_steady_decline_is_leak(self):
        result = run_m8.leak_check([10000, 9000, 8000, 7000, 6000, 5000], 0, 1024)
        self.assertEqual(result["verdict"], "leak")
        self.assertEqual(result["drop"], 5000)

    def test_leak_check_single_drop_is_stable(self):
        result = run_m8.leak_check([10000] * 3 + [5000] * 3, 0, 1024)
        self.assertEqual(result["verdict"], "stable")

    def test_leak_check_small_decline_within_tolerance_is_stable(self):
        result = run_m8.leak_check([1000, 990, 980, 970, 960, 950], 0, 1024)
        self.assertEqual(result["verdict"], "stable")

    def test_leak_check_warmup_excludes_early_samples(self):
        series = [9000, 5000, 4000, 4000, 4000, 4000, 4000]
        self.assertEqual(run_m8.leak_check(series, 2, 1024)["verdict"], "stable")

    def test_leak_check_too_few_samples(self):
        result = run_m8.leak_check([5, 4, 3, 2, 1], 2, 0)
        self.assertEqual(result, {"verdict": "insufficient", "samples": 3})

    def test_fnv1a_known_values(self):
        self.assertEqual(run_m8.fnv1a(""), 0x811c9dc5)
        self.assertEqual(run_m8.fnv1a("a"), 0xe40c292c)
        self.assertEqual(run_m8.fnv1a("foobar"), 0xbf9cf968)

    def test_monitor_fault_and_notice_classification(self):
        monitor = run_m8.Monitor(None, lambda: 5.0)
        monitor.received(b"Guru Meditation Error: Core 0 panic'ed (LoadProhibited)\r\n")
        monitor.received(b'!RE1 12345678 0 {"error":"Guru Meditation"}\n')
        monitor.received(b"rst:0x15 (USB_UART_CHIP_RESET),boot:0x8\n")
        monitor.received(b"plain log line\n")
        self.assertEqual([f["pattern"] for f in monitor.faults], ["Guru Meditation"])
        self.assertEqual([n["pattern"] for n in monitor.notices], ["rst:0x"])

    def test_monitor_assembles_lines_across_reads(self):
        monitor = run_m8.Monitor(None, lambda: 0.0)
        monitor.received(b"Task wat")
        monitor.received(b"chdog got triggered\n")
        self.assertEqual(len(monitor.faults), 1)

    def test_monitor_writes_traffic_to_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "serial.log"
            monitor = run_m8.Monitor(path, lambda: 0.0)
            monitor.sent(b"!RO1 00000001 STATUS\n")
            monitor.received(b"!RO1 00000001 0 {}\n")
            monitor.close()
            text = path.read_text()
        self.assertIn(">> !RO1 00000001 STATUS", text)
        self.assertIn("<< !RO1 00000001 0 {}", text)

    def test_discover_rejects_unrelated_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(run_m8.Blocked):
                run_m8.discover(None, None, Path(tmp) / "missing")


if __name__ == "__main__":
    unittest.main()
