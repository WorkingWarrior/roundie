#!/usr/bin/env python3
"""M8 unattended hardware validation of one Roundie board over USB Serial/JTAG.

Non-invasive by default. It reads diagnostics, stages example ELFs through the
RE1 staging file, injects UI input (reported as INJECTED, never as touch), turns
the screen off and on, syncs the RTC from the host and, where allowed, makes one
timed light sleep and one software restart. It never erases Flash, writes the
bootloader or partition table, touches eFuses, enters ROM Download, or resets
the chip through DTR/RTS. A firmware write happens only with --ota IMAGE, after
a passing preflight with a VALID rollback image in the other slot.

Statuses:
  PASS     checked on the device through its diagnostic protocols;
  FAIL     the device contradicted the expectation;
  BLOCKED  a precondition was not met or the step was unsafe, so it did not run;
  PENDING  needs a person (touch, sight, sound, buttons, current).

Reports: report.json and report.md in --out, rewritten after every step, so an
interrupted run leaves a partial report; serial.log holds all serial traffic.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import sys
import time
from datetime import datetime, timezone

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import roundie_budget as budget  # noqa: E402
import roundie_clock as clock  # noqa: E402
import roundie_elf as elf  # noqa: E402
import roundie_launcher as launcher  # noqa: E402
import roundie_ota as ota  # noqa: E402
import roundie_safe as safe  # noqa: E402
import roundie_power as power  # noqa: E402
import roundie_time as rtime  # noqa: E402
import roundie_ui as ui  # noqa: E402
from roundie_time import DeviceError, TransportError  # noqa: E402

RUNNER_VERSION = 1
PASS = "PASS"
FAIL = "FAIL"
BLOCKED = "BLOCKED"
PENDING = "PENDING"
BY_ID = Path("/dev/serial/by-id")
ESPRESSIF = re.compile(r"usb-Espressif_USB_JTAG_serial_debug_unit_([0-9A-Fa-f:]{17})-if00")
HOME = "org.roundie.watch"
BOARDS = ("1.85b", "1.75c")
DIALOG_TEXT = "Reset level to 40?\nReset\nCancel"
FAULTS = (b"Guru Meditation", b"abort() was called", b"Task watchdog", b"task_wdt",
          b"Interrupt wdt", b"Backtrace:", b"Stack canary", b"assert failed")
NOTICES = (b"rst:0x", b"BootGuard", b"Safe Mode", b"ESP-ROM:")
ELF_IDLE = 0
ELF_ACTIVE = 3
ELF_STOPPED = 11
MANUAL = (
    ("touch.panel", "Finger touch on list, slider, edge button, dialog and Exit swipe (CST816S)"),
    ("display.visual", "Panel content, repaint after screen on, round layout as seen by a person"),
    ("audio.audible", "Tone and stream audibility through the speaker"),
    ("buttons.boot", "BOOT press: power menu, screen on, wake from light sleep"),
    ("wake.touch", "Touch wake from light sleep"),
    ("power.current", "Current draw in active, screen-off and light-sleep states"),
    ("battery.physical", "Battery gauge against a connected or disconnected battery"),
)


class Blocked(Exception):
    """The step cannot run safely or its precondition is not met."""


def is_refusal(error):
    """True only when the loader rejected the file, not for a refused precondition."""
    return str(error).startswith("ELF rejected (")


def fnv1a(text):
    value = 2166136261
    for byte in text.encode():
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Monitor:
    """Serial log of everything sent and received, with fault detection."""

    def __init__(self, path=None, now=time.monotonic):
        self.file = open(path, "ab") if path else None
        self.now = now
        self.started = now()
        self.line = bytearray()
        self.faults = []
        self.notices = []

    def sent(self, data):
        self.write(b">> " + data.strip() + b"\n")

    def received(self, data):
        for byte in data:
            self.line.append(byte)
            if byte == 10 or len(self.line) >= 4096:
                self.finish(bytes(self.line))
                self.line.clear()

    def finish(self, line):
        self.write(b"<< " + line.rstrip(b"\r\n") + b"\n")
        if line.startswith(b"!"):
            return
        entry = {"at_s": round(self.now() - self.started, 3),
                 "line": line.decode("utf-8", errors="replace").strip()[:240]}
        for pattern in FAULTS:
            if pattern in line:
                self.faults.append(dict(entry, pattern=pattern.decode()))
                return
        for pattern in NOTICES:
            if pattern in line:
                self.notices.append(dict(entry, pattern=pattern.decode()))
                return

    def write(self, data):
        if self.file is not None:
            self.file.write(data)
            self.file.flush()

    def close(self):
        if self.file is not None:
            self.file.close()
            self.file = None


class TeePort:
    """Passes a serial port through, copying the traffic to the monitor."""

    def __init__(self, port, monitor):
        self.port = port
        self.monitor = monitor

    def write(self, data):
        self.monitor.sent(data)
        return self.port.write(data)

    def read(self, size=1):
        data = self.port.read(size)
        if data:
            self.monitor.received(data)
        return data


HANDLERS = {
    "ota": lambda port, operation, timeout: ota.request(port, operation, timeout or 8.0),
    "elf": lambda port, operation, timeout: elf.request(port, operation, timeout or 8),
    "ui": lambda port, operation, timeout: ui.request(port, operation, timeout or 4),
    "budget": lambda port, operation, timeout: budget.request(port, operation, timeout or 4),
    "launcher": lambda port, operation, timeout: launcher.request(port, operation, timeout or 4),
    "power": lambda port, operation, timeout: power.request(port, operation, timeout or 4.0),
    "time": lambda port, operation, timeout: rtime.request(port, operation, timeout or 5.0),
    "zone": lambda port, operation, timeout: rtime.zone_request(port, operation, timeout or 5.0),
    "clock": lambda port, operation, timeout: clock.query(port, timeout or 3.0),
    "safe": lambda port, operation, timeout: safe.request(port, operation, timeout or 8.0),
}


class Device:
    """One board at one stable path. Each call opens and closes the port."""

    def __init__(self, path, monitor, opener=None):
        self.path = str(path)
        self.monitor = monitor
        self.opener = opener or rtime.open_serial

    def session(self, function):
        with self.opener(self.path) as raw:
            return function(TeePort(raw, self.monitor))

    def call(self, protocol, operation="STATUS", timeout=None):
        return self.session(lambda port: HANDLERS[protocol](port, operation, timeout))


def discover(port=None, serial=None, by_id=BY_ID):
    """Return (path, serial) of the board, or raise Blocked. Never by ttyACM order."""
    candidates = {}
    if by_id.is_dir():
        for entry in sorted(by_id.iterdir()):
            match = ESPRESSIF.fullmatch(entry.name)
            if match is not None:
                candidates[match[1].upper()] = entry
    if serial is not None:
        serial = serial.upper()
    if port is not None:
        resolved = os.path.realpath(port)
        owners = [s for s, entry in candidates.items() if os.path.realpath(entry) == resolved]
        if serial is not None and serial not in owners:
            raise Blocked(f"port {port} is not the USB Serial/JTAG unit {serial}")
        if not owners and serial is None:
            raise Blocked(f"port {port} is not an Espressif USB Serial/JTAG unit; pass a /dev/serial/by-id path")
        return str(port), owners[0] if owners else serial
    if serial is not None:
        if serial not in candidates:
            raise Blocked(f"no Espressif USB Serial/JTAG unit with serial {serial}")
        return str(candidates[serial]), serial
    if len(candidates) != 1:
        raise Blocked(f"{len(candidates)} Espressif USB Serial/JTAG units found; pass --serial")
    serial, entry = next(iter(candidates.items()))
    return str(entry), serial


def released(unloaded, ui_status, instance):
    """Our instance left nothing behind. Home may already be loading again, so
    UI widgets count only while the UI generation is still ours or none."""
    if unloaded["state"] != ELF_IDLE or unloaded["resources"][3] != 0:
        return False
    if ui_status["generation"] in (0, instance):
        return ui_status["widgets"] == 0 and ui_status["outstanding"] == 0
    return True


def leak_check(series, warmup, tolerance):
    """A leak needs a steady decline past the warm-up, not a single drop."""
    steady = series[warmup:]
    if len(steady) < 5:
        return {"verdict": "insufficient", "samples": len(steady)}
    drop = steady[0] - steady[-1]
    declining = sum(1 for a, b in zip(steady, steady[1:]) if b < a)
    leak = drop > tolerance and declining >= 0.75 * (len(steady) - 1)
    return {"verdict": "leak" if leak else "stable", "first": steady[0], "last": steady[-1],
            "minimum": min(steady), "drop": drop, "declining_steps": declining,
            "steps": len(steady) - 1, "tolerance": tolerance}


class Runner:
    def __init__(self, args, device, monitor, out, sleep=time.sleep, now=time.monotonic):
        self.args = args
        self.device = device
        self.monitor = monitor
        self.out = Path(out)
        self.sleep = sleep
        self.now = now
        self.loaded = None
        self.home_initially = False
        self.screen_initially = None
        self.expected_resets = 0
        self.last_uptime_us = None
        self.diag = True
        self.disconnected = False
        self.halted = None
        self.app_info = {}
        self.report = {
            "runner": RUNNER_VERSION, "started": utc_now(), "finished": None,
            "host": {"system": platform.platform(), "python": platform.python_version()},
            "arguments": {k: str(v) for k, v in vars(args).items()},
            "device": {}, "preflight": {}, "ota": None, "steps": [],
            "monitoring": {"uptime": [], "unexpected_resets": [], "faults": [], "notices": [],
                           "tasks": "not exposed by any diagnostic protocol"},
            "manual": [{"name": n, "status": PENDING, "needs": text} for n, text in MANUAL],
            "restore": [], "outcome": None,
        }

    # ---- reporting -----------------------------------------------------
    def record(self, name, status, details, evidence="device protocol"):
        entry = {"name": name, "status": status, "evidence": evidence,
                 "at": utc_now(), "details": details}
        self.report["steps"].append(entry)
        self.write_report()
        return entry

    def write_report(self):
        self.report["monitoring"]["faults"] = list(self.monitor.faults)
        self.report["monitoring"]["notices"] = list(self.monitor.notices)
        self.out.mkdir(parents=True, exist_ok=True)
        for name, text in (("report.json", json.dumps(self.report, indent=2, sort_keys=False) + "\n"),
                           ("report.md", markdown(self.report))):
            temporary = self.out / (name + ".tmp")
            temporary.write_text(text)
            os.replace(temporary, self.out / name)

    # ---- device helpers ------------------------------------------------
    def call(self, protocol, operation="STATUS", timeout=None):
        return self.device.call(protocol, operation, timeout)

    def wait_for(self, what, probe, accept, timeout, interval=0.1):
        deadline = self.now() + timeout
        while True:
            value = probe()
            if accept(value):
                return value
            if self.now() >= deadline:
                raise DeviceError(f"timeout waiting for {what}")
            self.sleep(interval)

    def reconnect(self, timeout):
        """Passive reconnect: reopen the stable path until RO1 STATUS answers."""
        deadline = self.now() + timeout
        last = None
        while self.now() < deadline:
            try:
                return self.call("ota", "STATUS", 2.0)
            except (TransportError, OSError) as error:
                last = error
            self.sleep(0.5)
        raise TransportError(f"device did not return within {timeout} s: {last}")

    def observe_uptime(self, label):
        """Unexpected resets show up as a smaller uptime than the last sample."""
        if not self.diag:
            return None
        try:
            sample = self.call("clock", None)
        except (TransportError, DeviceError) as error:
            self.diag = False
            self.report["monitoring"]["uptime"].append({"label": label, "error": str(error)})
            return None
        uptime = sample["uptime_us"]
        reset = self.last_uptime_us is not None and uptime < self.last_uptime_us
        if reset:
            if self.expected_resets > 0:
                self.expected_resets -= 1
            else:
                self.report["monitoring"]["unexpected_resets"].append(
                    {"label": label, "before_us": self.last_uptime_us, "after_us": uptime})
        self.last_uptime_us = uptime
        self.report["monitoring"]["uptime"].append({"label": label, "uptime_us": uptime, "reset": reset})
        return uptime

    def app_path(self, name):
        path = Path(self.args.apps) / f"{name}.app.elf"
        if not path.is_file():
            raise Blocked(f"{path} is missing; build the examples (see --help)")
        return path

    def inspect(self, name):
        if name not in self.app_info:
            self.app_info[name] = elf.inspect(self.app_path(name))
        return self.app_info[name]

    def stage(self, name):
        path = self.app_path(name)
        info = self.inspect(name)
        self.device.session(lambda port: elf.upload(port, path, info))
        return info

    def start(self, name):
        info = self.stage(name)
        status = self.device.session(lambda port: elf.start(port, info["sha256"]))
        self.loaded = status["instance"]
        return status

    def stop(self):
        instance = self.loaded
        self.device.session(lambda port: elf.control(port, "STOP", instance))
        status = self.device.session(lambda port: elf.control(port, "UNLOAD", instance))
        self.loaded = None
        return status

    def settled_nav(self):
        """The launcher relaunches home after any application ends normally
        (components/roundie_launcher/launcher.c), so wait out its transitions."""
        return self.wait_for("settled launcher", lambda: self.call("launcher", "NAV"),
                             lambda n: n["state"] in ("LIST", "HOME", "ACTIVE", "RESULT", "ERROR"), 15, 0.2)

    def run_once(self, digest):
        """ABI 1 RUN. Home relaunches about 15 ms after the result appears, so
        poll without pauses; the result is valid only for our instance."""
        submitted = self.call("elf", f"RUN {digest}")
        instance = submitted["instance"]
        deadline = self.now() + 4
        while self.now() < deadline:
            status = self.call("elf", "STATUS", 1)
            if status["instance"] == instance and status["state"] == ELF_IDLE:
                return status
            if status["instance"] != instance and status["completed"] >= instance:
                raise DeviceError("application completed but its result was replaced before it was read")
        raise DeviceError("application still active; no forced unload attempted")

    def leave_home(self):
        # After boot the launcher shows LIST before it starts home, so a LIST
        # with a busy runtime is home starting: wait for it, then exit it.
        for _ in range(3):
            nav = self.settled_nav()
            if nav["state"] == "RESULT":
                self.call("launcher", f"BACK {nav['request']}")
                nav = self.settled_nav()
            if nav["state"] == "ACTIVE":
                self.call("launcher", f"EXIT {nav['generation']}")
                nav = self.wait_for("launcher list", lambda: self.call("launcher", "NAV"),
                                    lambda n: n["state"] == "LIST", 8)
            # M10: the face Home is idle like the list.
            if nav["state"] not in ("LIST", "HOME"):
                raise Blocked(f"launcher is {nav['state']}, not idle")
            try:
                self.wait_for("idle runtime", lambda: self.call("elf"),
                              lambda s: s["state"] == ELF_IDLE and not s["busy"], 5)
                return nav
            except DeviceError:
                # A loaded application keeps the runtime busy, so wait for the
                # launcher to report it, or for the runtime to fall idle.
                self.wait_for("home start", lambda: (self.call("launcher", "NAV"), self.call("elf")),
                              lambda pair: pair[0]["state"] == "ACTIVE" or
                              (pair[1]["state"] == ELF_IDLE and not pair[1]["busy"]), 15, 0.2)
        raise DeviceError("runtime did not become idle after leaving home three times")

    def return_home(self):
        nav = self.settled_nav()
        if nav["state"] == "HOME" or (nav["state"] == "ACTIVE" and nav["selected"] == HOME):
            return nav  # M10: the face Home
        if nav["state"] == "RESULT":
            self.call("launcher", f"BACK {nav['request']}")
            nav = self.settled_nav()
        if nav["state"] != "LIST":
            raise DeviceError(f"launcher is {nav['state']} ({nav['selected']}), cannot return home")
        self.call("launcher", f"SELECT {nav['revision']} {HOME}")
        return self.wait_for("home", lambda: self.call("launcher", "NAV"),
                             lambda n: n["state"] == "ACTIVE" and n["selected"] == HOME, 10)

    # ---- preflight -----------------------------------------------------
    def preflight(self, serial):
        checks = {}
        status = self.call("ota")
        checks["ota"] = status
        self.report["device"].update(serial=serial, path=self.device.path, firmware=status["version"],
                                     running=status["running"], elf=status["elf"])
        problems = []
        if status["ota_state"] != "VALID" or not status["checked"] or not status["passed"]:
            problems.append("running image is not self-tested VALID")
        if status["phase"] != "IDLE":
            problems.append(f"OTA phase is {status['phase']}")
        if status["boot"] != status["running"]:
            problems.append("boot selection differs from the running slot")
        recovery_status = self.call("safe", "STATUS")
        checks["safe"] = recovery_status
        if recovery_status["safe"]:
            problems.append(f"device is in Safe Mode (reason {recovery_status['reason']}); "
                            "leaving it is an operator decision (RS1 REBOOT)")
        recovery = status["other_image"] == 1 and status["other_state"] == "VALID" and status["rollback"] == 1
        checks["recovery"] = {"other_slot": status["other"], "other_state": status["other_state"],
                              "other_elf": status["other_elf"], "rollback": status["rollback"],
                              "usable": recovery}
        time_status = self.call("time", "GET")
        checks["time"] = time_status
        board = "1.75c" if time_status["state"] == rtime.NO_RTC else "1.85b"
        self.report["device"]["board_detected"] = board
        if self.args.board and self.args.board != board:
            problems.append(f"board is {board}, not {self.args.board} (RTC presence)")
        checks["elf"] = self.call("elf")
        if checks["elf"]["loader"] != "1.3.5":
            problems.append("unexpected loader")
        checks["launcher"] = self.call("launcher", "NAV")
        checks["ui"] = self.call("ui")
        checks["power"] = self.call("power")
        checks["zone"] = self.call("zone", None)
        self.home_initially = (checks["launcher"]["state"] == "ACTIVE" and checks["launcher"]["selected"] == HOME)
        self.screen_initially = checks["power"]["state"]
        self.report["device"]["profile"] = "rep" if checks["elf"]["abi"] in (0, 1, 2, 3) else "unknown"
        self.observe_uptime("preflight")
        self.report["preflight"] = {"status": FAIL if problems else PASS, "problems": problems,
                                    "recovery_path": recovery, "checks": checks}
        self.write_report()
        return not problems, recovery

    # ---- OTA -----------------------------------------------------------
    def update(self, image_path, recovery):
        result = {"image": str(image_path)}
        self.report["ota"] = result
        with open(image_path, "rb") as stream:
            image = ota.inspect_image(stream)
        result["image_info"] = image
        if not recovery:
            raise Blocked("no VALID rollback image in the other slot; OTA could need manual recovery")
        before = self.call("ota")
        result["before"] = {k: before[k] for k in ("version", "running", "elf", "other", "other_state")}
        if before["elf"] == image["elf"]:
            result["status"] = PASS
            result["note"] = "image already running; nothing written"
            return result
        self.leave_home()
        with open(image_path, "rb") as stream:
            ready = self.device.session(lambda port: ota.upload(port, stream, progress=lambda text: None))
        result["staged"] = {k: ready[k] for k in ("phase", "digest", "other_elf")}
        activated = self.call("ota", f"ACTIVATE {image['digest']}")
        if activated["phase"] != "ACTIVATION_PENDING" or activated["boot"] != activated["other"]:
            raise DeviceError("boot selection did not follow the staged image")
        self.expected_resets += 1
        try:
            self.call("ota", "RESTART")
        except TransportError:
            pass  # The reply can be lost when USB drops for the restart.
        self.sleep(2)
        after = self.wait_for("self-tested new image",
                              lambda: self.reconnect(60),
                              lambda s: s["elf"] == image["elf"] and s["checked"] == 1, 90, 1.0)
        ota.verify(after, image)
        if after["other_state"] != "VALID" or after["other_elf"] != before["elf"]:
            raise DeviceError("previous image is no longer the VALID rollback")
        self.observe_uptime("after OTA")
        result["after"] = {k: after[k] for k in ("version", "running", "elf", "other", "other_state", "ota_state")}
        result["status"] = PASS
        self.report["device"].update(firmware=after["version"], running=after["running"], elf=after["elf"])
        return result

    # ---- scenarios -----------------------------------------------------
    def s_core_status(self):
        details = {k: self.call(p) for k, p in (("ota", "ota"), ("elf", "elf"), ("ui", "ui"), ("power", "power"))}
        details["launcher"] = self.call("launcher", "NAV")
        problems = []
        if details["ui"]["error"] != 0 or details["ui"]["failed"] != 0 or details["ui"]["flush_errors"] != 0:
            problems.append("UI error, failed command or flush error")
        if details["elf"]["error"] or details["elf"]["over_budget"]:
            problems.append(f"ELF error {details['elf']['error']!r}")
        if details["ota"]["ota_state"] != "VALID":
            problems.append("running image not VALID")
        details["problems"] = problems
        return (FAIL if problems else PASS), details

    def s_rtc(self):
        before = self.call("time", "GET")
        zone = self.call("zone", None)
        details = {"before": before, "zone": zone}
        has_rtc = before["state"] != rtime.NO_RTC
        problems = []
        try:
            details["before_offset_s"] = rtime.require_valid(before)
        except DeviceError as error:
            problems.append(f"before sync: {error}")
        if has_rtc and before["rtc"] != -1 and before["wall"] != -1:
            details["before_rtc_minus_system_s"] = before["rtc"] - before["wall"]
        if not self.args.rtc_sync:
            details["note"] = "RT1 SET skipped (--no-rtc-sync)"
            return (FAIL if problems else PASS), details
        synced = self.call("time", "SET")
        details["set"] = synced
        after = self.call("time", "GET")
        details["after"] = after
        details["after_offset_s"] = rtime.require_valid(after)
        # SET does not touch the zone; a changed rule would be a device fault.
        zone_after = self.call("zone", None)
        details["zone_after"] = zone_after
        if zone_after != zone:
            problems.append(f"time zone changed from {zone} to {zone_after}")
        # TZ set and restore: another rule, then the user's rule back.
        try:
            details["zone_set"] = self.call("zone", "UTC0" if zone != "UTC0" else "CET-1CEST,M3.5.0,M10.5.0/3")
        finally:
            details["zone_restored"] = self.call("zone", zone)
            if details["zone_restored"] != zone:
                self.report["restore_failed"] = True
                problems.append("time zone rule was not restored")
            else:
                self.report["restore"].append(f"time zone rule {zone} restored")
        details["time_state_note"] = ("system time and RTC now follow host UTC (NTP); the earlier "
                                      "offset is recorded above, the zone rule is unchanged")
        if problems:
            details["problems"] = problems
            details["note"] = "pre-sync clock was wrong; SET corrected it (see before_offset_s)"
            return FAIL, details
        return PASS, details

    def s_launcher(self):
        details = {"initial": self.call("launcher", "NAV")}
        details["list"] = self.leave_home()
        refreshed = self.call("launcher", "REFRESH")
        details["refresh"] = self.wait_for("list after refresh", lambda: self.call("launcher", "NAV"),
                                           lambda n: n["state"] == "LIST" and n["revision"] >= refreshed["revision"], 8)
        details["home"] = self.return_home()
        problems = []
        if details["refresh"]["count"] == 0:
            problems.append("no packages enumerated")
        if details["home"]["generation"] <= details["initial"]["generation"]:
            problems.append("home generation did not advance on relaunch")
        details["origin"] = "INJECTED (RP1 EXIT/SELECT), not touch"
        details["problems"] = problems
        return (FAIL if problems else PASS), details

    def s_elf_hello(self):
        self.leave_home()
        info = self.stage("hello")
        result = self.run_once(info["sha256"])
        ok = result["app_result"] == 42 and result["abi"] == 1 and result["executed"] and not result["error"]
        return (PASS if ok else FAIL), {"sha256": info["sha256"], "result": result}

    def s_elf_event(self):
        self.leave_home()
        started = self.start("event")
        instance = started["instance"]
        before = self.call("elf")
        self.call("elf", f"EVENT {instance} 1234")
        after = self.wait_for("user event", lambda: self.call("elf"),
                              lambda s: s["users"] >= before["users"] + 1, 4)
        stopped = self.stop()
        ok = after["value"] == 1234 and after["error"] == "" and stopped["resources"][3] == 0
        return (PASS if ok else FAIL), {"started": started, "after_event": after, "unloaded": stopped}

    def ui_app(self, name, hold_s=2.0):
        self.leave_home()
        started = self.start(name)
        status = self.wait_for(f"{name} widgets", lambda: self.call("ui"),
                               lambda s: s["widgets"] > 0 and s["pending"] == 0, 4)
        self.sleep(hold_s)
        running = self.call("elf")
        ui_running = self.call("ui")
        stopped = self.stop()
        ui_after = self.call("ui")
        problems = []
        if running["error"] or running["state"] != ELF_ACTIVE:
            problems.append(f"application state {running['state']} error {running['error']!r}")
        if ui_running["failed"] or ui_running["error"]:
            problems.append("UI command failed")
        if not released(stopped, ui_after, started["instance"]):
            problems.append("resources or widgets left after unload")
        details = {"started": started, "ui": status, "running": running, "ui_running": ui_running,
                   "unloaded": stopped, "ui_after": ui_after, "problems": problems}
        return (FAIL if problems else PASS), details

    def s_elf_ui(self):
        return self.ui_app("ui")

    def s_elf_watch(self):
        return self.ui_app("watch", hold_s=2.5)

    def s_elf_stopwatch(self):
        return self.ui_app("stopwatch", hold_s=2.5)

    def round_handles(self, generation):
        status = self.wait_for("round widgets", lambda: self.call("ui"),
                               lambda s: s["generation"] == generation and s["widgets"] == 7 and s["pending"] == 0, 4)
        if status["text_hash"] != fnv1a(DIALOG_TEXT):
            raise DeviceError("last created widget is not the dialog; cannot derive handles")
        dialog = status["last_widget"]
        return status, {"color": dialog - 5, "level": dialog - 4, "list": dialog - 3,
                        "slider": dialog - 2, "edge": dialog - 1, "dialog": dialog}

    def inject(self, generation, operation, before, accept, what):
        self.call("ui", f"{operation}")
        return self.wait_for(what, lambda: (self.call("ui"), self.call("elf")),
                             lambda pair: accept(before, pair[0], pair[1]), 4)

    def s_round_dialog(self):
        """PR #4: a dialog choice must reach the executor and change application state."""
        self.leave_home()
        started = self.start("round")
        g = started["instance"]
        ui0, h = self.round_handles(g)
        rounds = []
        problems = []
        elf0 = self.call("elf")
        for i in range(self.args.dialog_rounds):
            level = 50 + i
            u, e = self.call("ui"), self.call("elf")
            u1, e1 = self.inject(g, f"VALUE {g} {h['slider']} {level}", (u, e),
                                 lambda b, uu, ee, level=level: uu["text_hash"] == fnv1a(f"Level: {level}") and
                                 ee["ui_events"] >= b[1]["ui_events"] + 1, "slider delivery")
            u2, e2 = self.inject(g, f"PRESS {g} {h['edge']}", (u1, e1),
                                 lambda b, uu, ee: ee["ui_events"] >= b[1]["ui_events"] + 1 and
                                 uu["applied"] >= b[0]["applied"] + 1 and uu["pending"] == 0, "dialog shown")
            u3, e3 = self.inject(g, f"VALUE {g} {h['dialog']} 1", (u2, e2),
                                 lambda b, uu, ee: uu["text_hash"] == fnv1a("Level: 40") and
                                 ee["ui_events"] >= b[1]["ui_events"] + 1, "dialog choice delivery")
            entry = {"level": level, "ui_events": [e["ui_events"], e3["ui_events"]],
                     "delivered": [u["delivered"], u3["delivered"]],
                     "rejected": [e["rejected"], e3["rejected"]],
                     "applied": [u["applied"], u3["applied"]],
                     "latency_max_us": u3["input_latency_max_us"]}
            if e3["rejected"] != e["rejected"]:
                problems.append(f"round {i}: executor rejected {e3['rejected'] - e['rejected']} event(s)")
            if e3["error"]:
                problems.append(f"round {i}: application error {e3['error']!r}")
            rounds.append(entry)
        # Cancel: delivered, and the level stays.
        u, e = self.call("ui"), self.call("elf")
        self.inject(g, f"PRESS {g} {h['edge']}", (u, e),
                    lambda b, uu, ee: ee["ui_events"] >= b[1]["ui_events"] + 1 and uu["pending"] == 0, "dialog shown")
        u, e = self.call("ui"), self.call("elf")
        uc, ec = self.inject(g, f"VALUE {g} {h['dialog']} 2", (u, e),
                             lambda b, uu, ee: ee["ui_events"] >= b[1]["ui_events"] + 1, "cancel delivery")
        if uc["text_hash"] != fnv1a("Level: 40") or ec["rejected"] != e["rejected"]:
            problems.append("cancel changed the level or was rejected")
        # Out of range: refused without delivery.
        u, e = self.call("ui"), self.call("elf")
        refused = False
        try:
            self.call("ui", f"VALUE {g} {h['slider']} 500")
        except DeviceError:
            refused = True
        self.sleep(0.3)
        u_oor, e_oor = self.call("ui"), self.call("elf")
        if e_oor["ui_events"] != e["ui_events"]:
            problems.append("an out-of-range slider value was delivered")
        elf_end = self.call("elf")
        stopped = self.stop()
        ui_after = self.call("ui")
        if not released(stopped, ui_after, g):
            problems.append("widgets or resources left after unload")
        details = {"generation": g, "handles": h, "created": ui0, "rounds": rounds,
                   "rejected_total": elf_end["rejected"] - elf0["rejected"],
                   "cancel": {"text_hash_level_40": uc["text_hash"] == fnv1a("Level: 40")},
                   "out_of_range": {"refused_by_command": refused, "input_rejected": u_oor["input_rejected"]},
                   "injected": u_oor["injected"], "physical": u_oor["physical"],
                   "unloaded": stopped, "ui_after": ui_after, "problems": problems,
                   "origin": "INJECTED through RE1 UI; not CST816S touch"}
        return (FAIL if problems else PASS), details

    def s_bad_package(self):
        self.leave_home()
        source = self.app_path("hello").read_bytes()
        broken = bytearray(source)
        broken[1:4] = b"BAD"
        path = self.out / "bad-magic.app.elf"
        path.write_bytes(bytes(broken))
        info = {"size": len(broken), "sha256": hashlib.sha256(broken).hexdigest()}
        details = {"variant": "ELF magic replaced", "sha256": info["sha256"]}
        # The board may refuse it when the upload ends (it validates the file
        # then) or, failing that, when it is started.
        try:
            self.device.session(lambda port: elf.upload(port, path, info))
            details["refused_at"] = "START"
            self.device.session(lambda port: elf.start(port, info["sha256"]))
            details["refused"] = False
        except DeviceError as error:
            details.setdefault("refused_at", "upload END")
            details["refused"] = is_refusal(error)
            details["refusal"] = str(error)[:300]
        state = self.wait_for("idle after refusal", lambda: self.call("elf"),
                              lambda s: s["state"] == ELF_IDLE and not s["busy"], 5)
        details["after"] = state
        unknown = "0" * 63 + "1"
        try:
            self.call("elf", f"START {unknown}")
            details["unknown_digest_refused"] = False
        except DeviceError:
            details["unknown_digest_refused"] = True
        # Recovery: a good application still runs.
        self.leave_home()
        hello = self.stage("hello")
        result = self.run_once(hello["sha256"])
        details["recovery_result"] = result["app_result"]
        ok = details["refused"] and details["unknown_digest_refused"] and result["app_result"] == 42
        if details.get("refused") is False and self.loaded is None:
            try:
                self.stop()
            except (DeviceError, TransportError):
                pass
        return (PASS if ok else FAIL), details

    def s_cycles(self):
        self.leave_home()
        samples = []
        problems = []
        for cycle in range(self.args.cycles):
            self.leave_home()
            started = self.start("round" if cycle % 2 == 0 else "ui")
            g = started["instance"]
            self.wait_for("widgets", lambda: self.call("ui"),
                          lambda s: s["generation"] == g and s["widgets"] > 0 and s["pending"] == 0, 4)
            stopped = self.stop()
            ui_after = self.call("ui")
            after = stopped["after"]
            sample = {"cycle": cycle, "instance": g, "internal_free": after[0], "internal_largest": after[1],
                      "psram_free": after[2], "exec_free": after[3], "exec_largest": after[4],
                      "load_us": stopped["load_us"], "unload_us": stopped["unload_us"],
                      "resources_outstanding": stopped["resources"][3], "widgets": ui_after["widgets"],
                      "ui_outstanding": ui_after["outstanding"], "lvgl_bytes": ui_after["lvgl_bytes"],
                      "error": stopped["error"], "dropped": stopped["dropped"]}
            samples.append(sample)
            if not released(stopped, ui_after, g) or sample["error"]:
                problems.append(f"cycle {cycle}: resources left or error")
            self.write_report()
        analysis = {key: leak_check([s[key] for s in samples], self.args.warmup, tolerance)
                    for key, tolerance in (("internal_free", 1024), ("internal_largest", 4096),
                                           ("psram_free", 4096), ("exec_free", 1024), ("lvgl_bytes", 256))}
        for key, result in analysis.items():
            if result["verdict"] == "leak":
                problems.append(f"{key} declines steadily: {result['drop']} B over {result['steps']} cycles")
        details = {"cycles": self.args.cycles, "warmup": self.args.warmup, "samples": samples,
                   "analysis": analysis, "problems": problems,
                   "tasks": "task count is not exposed by the diagnostics"}
        return (FAIL if problems else PASS), details

    def s_screen(self):
        initial = self.call("power")
        on = self.call("power", "SCREEN ON")
        self.wait_for("screen on", lambda: self.call("power"), lambda s: s["state"] == "active", 3)
        on_count = self.call("power")["screen_offs"]
        off = self.call("power", "SCREEN OFF")
        state_off = self.wait_for("screen off", lambda: self.call("power"),
                                  lambda s: s["state"] == "screen_off", 3)
        self.call("power", "SCREEN ON")
        state_on = self.wait_for("screen on", lambda: self.call("power"),
                                 lambda s: s["state"] == "active", 3)
        if initial["state"] == "screen_off":
            self.call("power", "SCREEN OFF")
            self.report["restore"].append("screen turned off again as found")
        ok = state_off["screen_offs"] == on_count + 1 and state_on["state"] == "active"
        return (PASS if ok else FAIL), {"initial": initial, "on": on, "off": off, "state_off": state_off,
                                        "state_on": state_on, "visual": "PENDING (person)"}

    def s_light_sleep(self):
        if not self.args.light_sleep:
            raise Blocked("timer wake is not verified on this board; pass --light-sleep to allow it")
        self.return_home()
        before = self.call("power")
        uptime_before = self.observe_uptime("before light sleep")
        reply = self.call("power", f"SLEEP {self.args.sleep_seconds}")
        self.sleep(1.0)
        after = self.wait_for("wake", lambda: self.safe_power_status(),
                              lambda s: s is not None and s["phase"] == "idle" and s["state"] != "light_sleep",
                              self.args.sleep_seconds + 40, 0.5)
        uptime_after = self.observe_uptime("after light sleep")
        home = self.wait_for("home after wake", lambda: self.call("launcher", "NAV"),
                             lambda n: n["state"] == "ACTIVE" and n["selected"] == HOME, 15, 0.5)
        problems = []
        if after["sleeps"] != before["sleeps"] + 1:
            problems.append(f"sleep not entered (refusal {after['refusal']})")
        if not 0.8 * self.args.sleep_seconds * 1000 <= after["last_sleep_ms"] <= 1.5 * self.args.sleep_seconds * 1000 + 1000:
            problems.append(f"slept {after['last_sleep_ms']} ms")
        if uptime_before is not None and uptime_after is not None and uptime_after < uptime_before:
            problems.append("the board reset instead of waking")
        return (FAIL if problems else PASS), {"before": before, "reply": reply, "after": after, "home": home,
                                              "problems": problems, "wake": "timer only; touch/BOOT wake PENDING"}

    def safe_power_status(self):
        try:
            return self.call("power", "STATUS", 2.0)
        except (TransportError, OSError):
            return None

    def s_restart(self):
        if not self.args.restart:
            raise Blocked("software restart disabled (--no-restart)")
        self.leave_home()
        before = self.call("ota")
        time_before = self.call("time", "GET")
        self.observe_uptime("before restart")
        started = self.now()
        self.expected_resets += 1
        try:
            self.call("ota", "RESTART")
        except TransportError:
            pass
        self.sleep(2)
        after = self.wait_for("self-test after restart", lambda: self.reconnect(60),
                              lambda s: s["checked"] == 1, 60, 0.5)
        uptime = self.observe_uptime("after restart")
        # Early in boot RT1 GET can answer before the RTC is read; record how long.
        first_time = self.call("time", "GET")
        time_after = self.wait_for("valid time after restart", lambda: self.call("time", "GET"),
                                   lambda t: t["code"] == "OK" and t["wall"] != -1, 20, 0.5)
        elapsed = self.now() - started
        home = self.wait_for("home autostart", lambda: self.call("launcher", "NAV"),
                             lambda n: n["state"] == "ACTIVE" and n["selected"] == HOME, 40, 1.0)
        problems = []
        if after["running"] != before["running"] or after["elf"] != before["elf"]:
            problems.append("a different image runs after restart")
        try:
            ota.verify(after)
        except DeviceError as error:
            problems.append(str(error))
        advance = rtime.reference(time_after) - rtime.reference(time_before)
        if time_after["wall"] == -1 or abs(advance - elapsed) > 3:
            problems.append(f"time across restart advanced {advance} s over {elapsed:.1f} s")
        if self.expected_resets:
            problems.append("no reset observed (uptime did not restart)")
            self.expected_resets = 0
        return (FAIL if problems else PASS), {"before": before, "after": after, "uptime_us": uptime,
                                              "time_before": time_before, "time_first_read": first_time,
                                              "time_after": time_after,
                                              "time_advance_s": advance, "host_elapsed_s": round(elapsed, 2),
                                              "home": home, "problems": problems}

    SCENARIOS = ("core_status", "rtc", "launcher", "elf_hello", "elf_event", "elf_ui", "elf_watch",
                 "elf_stopwatch", "round_dialog", "bad_package", "cycles", "screen", "light_sleep", "restart")

    def run_scenario(self, name):
        if self.disconnected:
            return self.record(name, BLOCKED, {"reason": "device unavailable after an earlier loss"})
        if self.halted:
            return self.record(name, BLOCKED, {"reason": f"run halted: {self.halted}"})
        monitoring = self.report["monitoring"]
        faults_before = len(self.monitor.faults)
        resets_before = len(monitoring["unexpected_resets"])
        evidence = "device protocol; UI input INJECTED" if name in ("round_dialog", "launcher") else "device protocol"
        started = self.now()
        try:
            status, details = getattr(self, "s_" + name)()
        except Blocked as error:
            status, details = BLOCKED, {"reason": str(error)}
        except DeviceError as error:
            status, details = FAIL, {"error": str(error)}
        except (TransportError, OSError) as error:
            status, details = FAIL, {"error": f"communication: {error}"}
            try:
                self.reconnect(self.args.reconnect_timeout)
                details["reconnected"] = True
            except TransportError as lost:
                details["reconnected"] = False
                details["lost"] = str(lost)
                self.disconnected = True
        details["duration_s"] = round(self.now() - started, 2)
        if self.loaded is not None and not self.disconnected:
            try:
                self.stop()
                details["cleanup"] = "stopped the application left by this step"
            except (DeviceError, TransportError) as error:
                details["cleanup_error"] = str(error)
        if not self.disconnected:
            self.observe_uptime(f"after {name}")
        # A panic or reset changes the device state (BootGuard may enter Safe
        # Mode), so later steps would only report its consequences.
        if len(self.monitor.faults) > faults_before or len(monitoring["unexpected_resets"]) > resets_before:
            self.halted = f"serial fault or unexpected reset during {name}"
            status = FAIL
            details["halted"] = self.halted
        return self.record(name, status, details, evidence)

    def restore(self):
        """Leave the board as found: no application of ours, home, screen state."""
        if self.disconnected:
            self.report["restore"].append("skipped: device unavailable")
            return
        if self.halted:
            self.report["restore"].append(f"skipped: {self.halted}; the device state needs review")
            self.report["restore_failed"] = True
            return
        try:
            if self.loaded is not None:
                self.stop()
                self.report["restore"].append("stopped and unloaded our application")
            if self.home_initially:
                self.return_home()
                self.report["restore"].append("home application running")
            if self.screen_initially == "screen_off":
                status = self.call("power")
                if status["state"] == "active":
                    self.call("power", "SCREEN OFF")
                    self.report["restore"].append("screen off as found")
        except (DeviceError, TransportError, Blocked) as error:
            self.report["restore"].append(f"FAILED: {error}")
            self.report["restore_failed"] = True


def markdown(report):
    lines = [f"# M8 hardware validation — {report['device'].get('board_detected', '?')}", ""]
    device = report["device"]
    lines += [f"- Started {report['started']}, finished {report['finished'] or 'not finished'}",
              f"- Device: serial `{device.get('serial')}`, path `{device.get('path')}`",
              f"- Firmware {device.get('firmware')} in {device.get('running')}, ELF `{str(device.get('elf'))[:16]}…`",
              f"- Preflight: {report['preflight'].get('status', 'not run')}"
              + (f" ({'; '.join(report['preflight'].get('problems', []))})" if report['preflight'].get('problems') else ""),
              f"- Outcome: {report['outcome'] or 'in progress'}", ""]
    if report["ota"]:
        lines += [f"- OTA: {report['ota'].get('status')} {report['ota'].get('note', '')}", ""]
    lines += ["| Step | Status | Evidence | Notes |", "|---|---|---|---|"]
    for step in report["steps"]:
        details = step["details"]
        note = details.get("reason") or details.get("error") or "; ".join(details.get("problems", []))
        lines.append(f"| {step['name']} | {step['status']} | {step['evidence']} | {str(note)[:160]} |")
    for item in report["manual"]:
        lines.append(f"| {item['name']} | {item['status']} | needs a person | {item['needs']} |")
    monitoring = report["monitoring"]
    lines += ["", f"- Unexpected resets: {len(monitoring['unexpected_resets'])}",
              f"- Serial faults (panic, watchdog, assert): {len(monitoring['faults'])}",
              f"- Serial notices (reset banners, BootGuard, Safe Mode): {len(monitoring['notices'])}",
              f"- Task count: {monitoring['tasks']}",
              f"- Restore: {'; '.join(report['restore']) or 'nothing to restore'}", ""]
    for fault in monitoring["faults"][:20]:
        lines.append(f"  - fault at {fault['at_s']} s: `{fault['line']}`")
    return "\n".join(lines) + "\n"


def outcome(report):
    statuses = [step["status"] for step in report["steps"]]
    if report["preflight"].get("status") != PASS:
        return BLOCKED
    if FAIL in statuses or report["monitoring"]["unexpected_resets"] or report["monitoring"]["faults"]:
        return FAIL
    if report.get("restore_failed"):
        return FAIL
    if BLOCKED in statuses:
        return BLOCKED
    return PASS


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="explicit /dev/serial/by-id path; default: discover the single Espressif unit")
    parser.add_argument("--serial", help="expected USB serial (MAC), e.g. 28:84:85:87:98:7C")
    parser.add_argument("--board", choices=BOARDS, help="expected board; checked by RTC presence")
    parser.add_argument("--out", default=None, help="report directory (default build-ci/hardware-validation/<UTC>)")
    parser.add_argument("--apps", default=str(ROOT / "build-ci/m8-apps"),
                        help="directory with <name>.app.elf examples built for esp32s3")
    parser.add_argument("--only", nargs="+", choices=Runner.SCENARIOS, help="run only these steps")
    parser.add_argument("--skip", nargs="+", choices=Runner.SCENARIOS, default=[], help="skip these steps")
    parser.add_argument("--cycles", type=int, default=20, help="load/unload cycles (default 20)")
    parser.add_argument("--warmup", type=int, default=3, help="cycles excluded from leak analysis")
    parser.add_argument("--dialog-rounds", type=int, default=10, help="slider/dialog rounds in round_dialog")
    parser.add_argument("--no-rtc-sync", dest="rtc_sync", action="store_false", help="do not RT1 SET")
    parser.add_argument("--light-sleep", action="store_true", help="allow one timed light sleep (timer wake)")
    parser.add_argument("--sleep-seconds", type=int, default=5)
    parser.add_argument("--no-restart", dest="restart", action="store_false", help="skip the software restart")
    parser.add_argument("--ota", type=Path, help="install this roundie_rep.bin first (firmware write)")
    parser.add_argument("--reconnect-timeout", type=float, default=30.0)
    args = parser.parse_args(argv)
    if not 1 <= args.cycles <= 500 or not 0 <= args.warmup < args.cycles:
        parser.error("--cycles 1..500 and --warmup below it")
    if not 1 <= args.sleep_seconds <= 60:
        parser.error("--sleep-seconds 1..60")
    if not 1 <= args.dialog_rounds <= 200:
        parser.error("--dialog-rounds 1..200")
    return args


def main(argv=None, device_factory=Device, by_id=BY_ID, sleep=time.sleep, now=time.monotonic):
    args = parse_args(argv)
    out = Path(args.out or ROOT / "build-ci/hardware-validation" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    out.mkdir(parents=True, exist_ok=True)
    monitor = Monitor(out / "serial.log", now)
    runner = None

    def terminate(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        try:
            path, serial = discover(args.port, args.serial, by_id)
        except Blocked as error:
            dummy = Runner(args, None, monitor, out, sleep, now)
            dummy.report["preflight"] = {"status": BLOCKED, "problems": [str(error)]}
            dummy.report["outcome"] = BLOCKED
            dummy.report["finished"] = utc_now()
            dummy.write_report()
            print(f"BLOCKED: {error}", file=sys.stderr)
            return 2
        runner = Runner(args, device_factory(path, monitor), monitor, out, sleep, now)
        try:
            ready, recovery = runner.preflight(serial)
        except (TransportError, DeviceError, OSError) as error:
            runner.report["preflight"] = {"status": BLOCKED, "problems": [f"no valid Roundie reply: {error}"]}
            ready = False
        if not ready:
            runner.report["outcome"] = BLOCKED
            return 2
        if args.ota is not None:
            try:
                runner.report["ota"] = runner.update(args.ota, recovery)
            except Blocked as error:
                runner.report["ota"] = {"status": BLOCKED, "reason": str(error)}
            except (DeviceError, TransportError, OSError) as error:
                runner.report["ota"] = dict(runner.report["ota"] or {}, status=FAIL, error=str(error))
            runner.write_report()
            if runner.report["ota"]["status"] != PASS:
                runner.report["outcome"] = runner.report["ota"]["status"]
                return 1
        names = args.only or Runner.SCENARIOS
        for name in names:
            if name in args.skip:
                continue
            runner.run_scenario(name)
        runner.restore()
        runner.report["outcome"] = outcome(runner.report)
        return {PASS: 0, BLOCKED: 2}.get(runner.report["outcome"], 1)
    except KeyboardInterrupt:
        if runner is not None:
            runner.report["interrupted"] = True
            runner.restore()
            runner.report["outcome"] = "INTERRUPTED"
        return 130
    finally:
        signal.signal(signal.SIGTERM, previous)
        if runner is not None:
            runner.report["finished"] = utc_now()
            runner.write_report()
            print(markdown(runner.report))
        monitor.close()


if __name__ == "__main__":
    sys.exit(main())
