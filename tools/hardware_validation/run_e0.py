#!/usr/bin/env python3
"""E0 baseline of one Roundie board over USB Serial/JTAG (docs/roadmap.md, E0).

Collects memory, stack, GUI/DMA and LittleFS figures for Home, the installed
stopwatch, a full refresh, the state after closing the stopwatch and one timed
light sleep, plus LittleFS read and SHA-256 timing (RP1 FSBENCH) for 64, 512
and 1024 KiB. It reuses the discovery, serial monitor and device session of
run_m8.py and the existing protocols (RS1, RP1, RE1, RW1, RG1, RO1, RT1).

Safety: no Flash erase, no partition, bootloader, NVS or eFuse writes, no deep
sleep, no ROM Download, no DTR/RTS reset. The only Flash writes are the FSBENCH
test files in /storage/dev (removed again) and, with --ota IMAGE, one OTA
image after a passing preflight with a VALID rollback image.

Statuses: PASS (checked through a protocol), FAIL (checked and wrong), BLOCKED
(precondition missing), PENDING (needs a person or an instrument: current,
touch, panel image, audio, wake time against an independent clock).
"""
import argparse
import hashlib
import json
import os
import re
import secrets
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_m8 as m8  # noqa: E402
from run_m8 import BLOCKED, FAIL, PASS, PENDING, Blocked  # noqa: E402
import roundie_display as display  # noqa: E402
import roundie_gauge as gauge  # noqa: E402
import roundie_package as package  # noqa: E402
from roundie_time import DeviceError, TransportError, read_serial_reply, write_command  # noqa: E402

RUNNER_VERSION = 2
STOPWATCH = "org.roundie.stopwatch"
# Creation order in examples/stopwatch_elf/stopwatch.c initialize().
STOPWATCH_ROLES = ("view", "dial", "time", "laps", "lap")
FS_SIZES_KIB = (64, 512, 1024)
FS_SEED = 0x9E3779B9  # main/storage_bench.c BENCH_SEED
FS_CHUNK = 4096
# The default brightness after boot, from source; used only when RW1 STATUS
# predates its brightness field.
BRIGHTNESS_SOURCE = {"percent": 60, "source": "main/main.c gui_state_t default; not read back"}
# RG1 BATTERY board names (roundie_board_config.h) for each --board choice.
BOARD_NAMES = {"1.85b": "Waveshare ESP32-S3-Touch-LCD-1.85B", "1.75c": "Waveshare ESP32-S3-Touch-AMOLED-1.75C"}
POWER_CHIPS = {"1.85b": "BQ27220 fuel gauge", "1.75c": "AXP2101 PMIC"}
PENDING_ITEMS = (
    ("current.active", "Average and peak current with the screen on (Home, stopwatch)"),
    ("current.screen_off", "Average current with the screen off"),
    ("current.light_sleep", "Average current in light sleep"),
    ("current.peaks", "Peaks at wake and at start"),
    ("touch.finger", "Finger touch on the stopwatch and Home"),
    ("display.visual", "Panel image, full refresh as seen by a person"),
    ("audio.audible", "Audibility"),
    ("wake.time", "Wake moment against an independent clock (USB return is not the wake moment)"),
)


# roundie_power_inhibit.h: ROUNDIE_POWER_INHIBIT_DISPLAY_AWAKE; held for at most
# the firmware maximum, released explicitly at the end of the run.
DISPLAY_AWAKE = 2
SCREEN_HOLD_S = 600


def fs_pattern_sha256(kib):
    """SHA-256 of the FSBENCH test file: xorshift32 words, little-endian."""
    digest = hashlib.sha256()
    state = FS_SEED
    words = bytearray(FS_CHUNK)
    for _ in range(kib * 1024 // FS_CHUNK):
        for i in range(0, FS_CHUNK, 4):
            state ^= (state << 13) & 0xFFFFFFFF
            state ^= state >> 17
            state ^= (state << 5) & 0xFFFFFFFF
            words[i:i + 4] = state.to_bytes(4, "little")
        digest.update(words)
    return digest.hexdigest()


def fsbench_request(port, operation, timeout=30.0):
    """RP1 FSBENCH PREP|READ|DEL <64|512|1024>. Returns the reply with its code."""
    if re.fullmatch(r"(?:PREP|READ|DEL) (?:64|512|1024)", operation) is None:
        raise DeviceError("invalid FSBENCH operation")
    identity = secrets.token_hex(4)
    prefix = f"!RP1 {identity} ".encode()
    write_command(port, f"!RP1 {identity} FSBENCH {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 400)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        if re.fullmatch(rb"-?[0-9]{1,10}", code) is None:
            raise ValueError("code")
        result = json.loads(payload)
        if result["format"] != 1 or result["fsbench"] != operation.split()[0]:
            raise ValueError("format")
        for key in ("kib", "bytes", "write_us", "read_us", "sha_us", "total_us", "chunk_max_us"):
            if type(result[key]) is not int or result[key] < 0:
                raise ValueError(key)
        result["code"] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed FSBENCH reply: {error}") from error
    return result


HANDLERS = dict(m8.HANDLERS)
HANDLERS.update({
    "perf": lambda port, operation, timeout: display.request(port, operation, timeout or 4, allow_error=True),
    "package": lambda port, operation, timeout: package.request(port, operation, timeout or 8),
    "gauge": lambda port, operation, timeout: gauge.request(port, operation, timeout or 15.0),
    "fsbench": lambda port, operation, timeout: fsbench_request(port, operation, timeout or 30.0),
})


class Device(m8.Device):
    def call(self, protocol, operation="STATUS", timeout=None):
        return self.session(lambda port: HANDLERS[protocol](port, operation, timeout))


def stopwatch_roles(ui_status, generation):
    """Semantic handles of the stopwatch from its creation order
    (examples/stopwatch_elf/stopwatch.c). The bridge assigns handles from one
    serial counter and reports the root view; the other four follow it.
    (last_widget is the handle of the last applied command, not the last created.)"""
    if ui_status["generation"] != generation or ui_status["widgets"] != len(STOPWATCH_ROLES):
        raise DeviceError(f"stopwatch UI not ready: {ui_status['widgets']} widgets, "
                          f"generation {ui_status['generation']} != {generation}")
    first = ui_status["view"]
    if first < 1:
        raise DeviceError("stopwatch handles out of range")
    return {role: first + i for i, role in enumerate(STOPWATCH_ROLES)}


def semantic_operation(kind, generation, roles, role, value=None):
    """RE1 UI PRESS/VALUE addressed by role instead of a raw widget handle."""
    if role not in roles:
        raise DeviceError(f"unknown role {role!r}")
    if kind == "PRESS":
        return f"PRESS {generation} {roles[role]}"
    if kind == "VALUE" and isinstance(value, int):
        return f"VALUE {generation} {roles[role]} {value}"
    raise DeviceError(f"invalid semantic operation {kind}")


def gui_window(traces, after_sequence):
    """Statistics of the retained GUI iterations after a sequence number."""
    window = [t for t in traces if t["sequence"] > after_sequence]
    flushing = [t for t in window if t["flushes"] > 0]
    result = {"iterations": len(window), "flushing": len(flushing),
              "truncated": bool(window) and window[0]["sequence"] != after_sequence + 1}
    for key in ("dma_us", "render_us", "submit_us", "wait_us", "handler_us", "scale_us"):
        values = [t[key] for t in flushing]
        if values:
            result[key] = {"max": max(values), "mean": round(sum(values) / len(values), 1)}
    return result


def heap_figures(health):
    return {key: health[key] for key in ("internal_free", "internal_min", "internal_largest",
                                         "psram_free", "psram_min", "psram_largest", "alloc_failed")}


def heap_delta(before, after):
    return {key: after[key] - before[key] for key in before}


def brightness_context(power_status):
    """Backlight percent as RW1 STATUS reports it (applied while the screen is on)."""
    if "brightness" not in power_status:
        return BRIGHTNESS_SOURCE
    return {"percent": power_status["brightness"], "source": "RW1 STATUS brightness (GUI setting)"}


def power_context(board, battery):
    """Power source and charging state from RG1 BATTERY: the chip's state, not a
    current measurement (the E0 current gate items stay PENDING)."""
    power = {"source": f"RG1 BATTERY ({POWER_CHIPS[board]} state, not a current measurement)",
             "battery_present": bool(battery["present"]), "charging": bool(battery["charging"]),
             "discharging": bool(battery["discharging"]), "state": battery["state"]}
    if battery["voltage_valid"]:
        power["voltage_mv"] = battery["voltage_mv"]
    if battery["soc_valid"]:
        power["soc"] = battery["soc"]
    if battery["current_valid"]:
        power["current_ma"] = battery["current_ma"]
    if not battery["present"]:
        power["supply"] = "USB only (no battery reported)"
    elif battery["charging"]:
        power["supply"] = "USB, battery charging"
    elif battery["discharging"]:
        power["supply"] = "battery discharging (USB data link present)"
    else:
        power["supply"] = "USB, battery idle or charge complete"
    return power


class Runner:
    def __init__(self, args, device, monitor, out, sleep=time.sleep, now=time.monotonic):
        self.args = args
        self.device = device
        self.monitor = monitor
        self.out = Path(out)
        self.sleep = sleep
        self.now = now
        self.last_uptime_us = None
        self.expected_resets = 0
        self.halted = None
        self.snapshots = {}
        self.report = {
            "runner": RUNNER_VERSION, "started": m8.utc_now(), "finished": None,
            "arguments": {k: str(v) for k, v in vars(args).items()},
            "context": {}, "ota": None, "steps": [], "snapshots": self.snapshots,
            "monitoring": {"uptime": [], "unexpected_resets": [], "faults": [], "notices": []},
            "pending": [{"name": n, "status": PENDING, "needs": text} for n, text in PENDING_ITEMS],
            "outcome": None,
        }

    # ---- reporting -----------------------------------------------------
    def record(self, name, status, details, evidence="device protocol"):
        entry = {"name": name, "status": status, "evidence": evidence, "at": m8.utc_now(), "details": details}
        self.report["steps"].append(entry)
        self.write_report()
        return entry

    def write_report(self):
        self.report["monitoring"]["faults"] = list(self.monitor.faults)
        self.report["monitoring"]["notices"] = list(self.monitor.notices)
        self.out.mkdir(parents=True, exist_ok=True)
        temporary = self.out / "report.json.tmp"
        temporary.write_text(json.dumps(self.report, indent=2) + "\n")
        os.replace(temporary, self.out / "report.json")

    # ---- device helpers ------------------------------------------------
    def call(self, protocol, operation="STATUS", timeout=None):
        return self.device.call(protocol, operation, timeout)

    def wait_for(self, what, probe, accept, timeout, interval=0.2):
        deadline = self.now() + timeout
        while True:
            value = probe()
            if accept(value):
                return value
            if self.now() >= deadline:
                raise DeviceError(f"timeout waiting for {what}")
            self.sleep(interval)

    def probe_quietly(self, protocol, operation="STATUS", timeout=2.0):
        try:
            return self.call(protocol, operation, timeout)
        except (TransportError, OSError):
            return None

    def observe_uptime(self, label):
        health = self.call("safe", "HEALTH")
        uptime = health["uptime_us"]
        reset = self.last_uptime_us is not None and uptime < self.last_uptime_us
        if reset:
            if self.expected_resets > 0:
                self.expected_resets -= 1
            else:
                self.report["monitoring"]["unexpected_resets"].append(
                    {"label": label, "before_us": self.last_uptime_us, "after_us": uptime})
        self.last_uptime_us = uptime
        self.report["monitoring"]["uptime"].append({"label": label, "uptime_us": uptime, "reset": reset})
        return health

    def nav(self):
        return self.call("launcher", "NAV")

    def settled_nav(self):
        return self.wait_for("settled launcher", self.nav,
                             lambda n: n["state"] in ("LIST", "HOME", "ACTIVE", "RESULT", "ERROR"), 15)

    def go_home(self):
        """Home is the system watch face (launcher HOME)."""
        nav = self.settled_nav()
        if nav["state"] == "HOME":
            return nav
        if nav["state"] == "RESULT":
            self.call("launcher", f"BACK {nav['request']}")
            nav = self.settled_nav()
        if nav["state"] == "ACTIVE":
            self.call("launcher", f"EXIT {nav['generation']}")
            nav = self.wait_for("Home or list after exit", self.nav, lambda n: n["state"] in ("HOME", "LIST"), 10)
        if nav["state"] in ("LIST", "ERROR"):
            self.call("launcher", "HOME")
        nav = self.wait_for("Home", self.nav, lambda n: n["state"] == "HOME", 10)
        # Runtime admission (and LittleFS usage) arrives some seconds after boot;
        # snapshots taken before it report capacity ok=0.
        self.wait_for("runtime admission", lambda: self.call("package"),
                      lambda status: status["capacity"][0] == 1, 60, interval=1.0)
        return nav

    def open_list(self):
        """RP1 OPEN from Home. Without a list snapshot since boot the launcher
        shows ERROR ("Lista niedostępna"); the user path is Refresh."""
        self.call("launcher", "OPEN")
        nav = self.wait_for("list", self.nav, lambda n: n["state"] in ("LIST", "ERROR"), 10)
        if nav["state"] == "ERROR":
            self.call("launcher", "REFRESH")
            nav = self.wait_for("refreshed list", self.nav, lambda n: n["state"] == "LIST", 20)
        return nav

    def perf_traces(self, after_sequence):
        """Retained GUI iterations newer than after_sequence, in one session."""
        def read(port):
            first = display.request(port, "PERF", 4)
            traces = []
            cursor = max(after_sequence, first["oldest"] - 1 if first["oldest"] else 0)
            while cursor < first["latest"]:
                reply = display.request(port, f"PERF {cursor}", 4)
                trace = reply["trace"]
                if trace["sequence"] <= cursor:
                    break
                traces.append(trace)
                cursor = trace["sequence"]
            return first, traces
        return self.device.session(read)

    def snapshot(self, label, perf_after=None):
        health = self.observe_uptime(label)
        perf = self.call("perf", "PERF")
        status = self.call("package", "STATUS")
        ui_status = self.call("ui")
        entry = {"at": m8.utc_now(), "nav": self.nav()["state"], "heap": heap_figures(health),
                 "stacks": health["stacks"], "executor_guard": health.get("executor_guard"),
                 "tasks": health["tasks"], "gui_summary": perf["summary"],
                 "littlefs": {"ok": status["capacity"][0], "total": status["capacity"][1],
                              "used": status["capacity"][2],
                              "free": status["capacity"][1] - status["capacity"][2]},
                 "ui": {k: ui_status[k] for k in ("generation", "widgets", "outstanding", "lvgl_bytes", "lvgl_peak")}}
        if perf_after is not None:
            _, traces = self.perf_traces(perf_after)
            entry["gui_window"] = gui_window(traces, perf_after)
        self.snapshots[label] = entry
        self.write_report()
        return entry

    def settle(self, seconds):
        self.sleep(seconds)

    # ---- context and OTA -----------------------------------------------
    def context(self, serial):
        ota_status = self.call("ota")
        safe_status = self.call("safe", "STATUS")
        time_status = self.call("time", "GET")
        power_status = self.call("power")
        context = {
            "board": self.args.board, "usb_serial": serial, "path": self.device.path,
            "source_commit": self.args.commit, "firmware": ota_status["version"],
            "running": ota_status["running"], "elf_sha256": ota_status["elf"],
            "ota_state": ota_status["ota_state"], "safe": safe_status["safe"], "safe_reason": safe_status["reason"],
            "rtc": {"state": time_status["state"], "code": time_status["code"]},
            "usb": "connected (the runner talks over USB Serial/JTAG)",
            "brightness": brightness_context(power_status), "screen": power_status["state"],
        }
        problems = []
        try:
            battery = self.call("gauge", "BATTERY")
            context["board_reported"] = battery["board"]
            context["display"] = {"panel": battery["display"], "canvas": [360, 360],
                                  "upscaled": battery["display"] != [360, 360]}
            context["power"] = power_context(self.args.board, battery)
            if battery["board"] != BOARD_NAMES[self.args.board]:
                problems.append(f"the firmware reports board {battery['board']!r}, not {self.args.board}")
        except (DeviceError, TransportError) as error:
            context["power"] = {"source": "RG1 BATTERY", "error": str(error)}
            problems.append(f"board identity unknown (RG1 BATTERY: {error})")
        if self.args.board == "1.85b":
            try:
                g = self.call("gauge", "STATUS")
                context["gauge"] = {"source": "RG1 STATUS (BQ27220 state, not a current measurement)",
                                    "voltage_mv": g["voltage_mv"], "current_ma": g["current_ma"],
                                    "soc": g["soc"], "full_mah": g["full_mah"], "battery_flags": g["battery_flags"]}
            except (DeviceError, TransportError) as error:
                context["gauge"] = {"source": "RG1 STATUS", "error": str(error)}
        if self.args.elf:
            local = hashlib.sha256(Path(self.args.elf).read_bytes()).hexdigest()
            context["local_elf_sha256"] = local
            context["elf_matches_build"] = local == ota_status["elf"]
        self.report["context"] = context
        self.write_report()
        if safe_status["safe"]:
            problems.append(f"Safe Mode ({safe_status['reason']})")
        if ota_status["ota_state"] != "VALID" or ota_status["phase"] != "IDLE":
            problems.append("running image is not VALID/IDLE")
        if self.args.elf and not context["elf_matches_build"]:
            problems.append("running ELF differs from --elf")
        return problems

    def update(self, image_path):
        with open(image_path, "rb") as stream:
            image = m8.ota.inspect_image(stream)
        before = self.call("ota")
        result = {"image": str(image_path), "before": {k: before[k] for k in ("version", "running", "elf")}}
        self.report["ota"] = result
        if before["elf"] == image["elf"]:
            result.update(status=PASS, note="image already running; nothing written")
            return result
        if not (before["other_image"] == 1 and before["other_state"] == "VALID" and before["rollback"] == 1):
            raise Blocked("no VALID rollback image in the other slot")
        nav = self.settled_nav()
        if nav["state"] == "ACTIVE":
            self.call("launcher", f"EXIT {nav['generation']}")
            self.wait_for("idle launcher", self.nav, lambda n: n["state"] in ("HOME", "LIST"), 10)
        with open(image_path, "rb") as stream:
            self.device.session(lambda port: m8.ota.upload(port, stream, progress=lambda text: None))
        activated = self.call("ota", f"ACTIVATE {image['digest']}")
        if activated["phase"] != "ACTIVATION_PENDING":
            raise DeviceError("activation not pending")
        # No uptime sample precedes the OTA, so its reset is not counted.
        try:
            self.call("ota", "RESTART")
        except TransportError:
            pass
        self.sleep(3)
        after = self.wait_for("self-tested new image", lambda: self.probe_quietly("ota", "STATUS"),
                              lambda s: s is not None and s["elf"] == image["elf"] and s["checked"] == 1, 120, 1.0)
        m8.ota.verify(after, image)
        result.update(status=PASS, after={k: after[k] for k in ("version", "running", "elf", "ota_state")})
        self.write_report()
        return result

    # ---- scenarios -----------------------------------------------------
    def s_home(self):
        self.go_home()
        perf = self.call("perf", "PERF")
        self.settle(self.args.settle)
        entry = self.snapshot("home", perf_after=perf["latest"])
        return PASS, {"snapshot": "home", "nav": entry["nav"]}

    def s_stopwatch(self):
        nav = self.go_home()
        perf = self.call("perf", "PERF")
        nav = self.open_list()
        self.call("launcher", f"SELECT {nav['revision']} {STOPWATCH}")
        nav = self.wait_for("stopwatch active", self.nav,
                            lambda n: n["state"] == "ACTIVE" and n["selected"] == STOPWATCH, 15)
        generation = nav["generation"]
        ui_status = self.wait_for("stopwatch widgets", lambda: self.call("ui"),
                                  lambda s: s["generation"] == generation and s["widgets"] == 5 and s["pending"] == 0, 6)
        roles = stopwatch_roles(ui_status, generation)
        problems = []
        idle = self.call("ui")
        self.call("ui", semantic_operation("PRESS", generation, roles, "dial"))
        self.settle(2.0)
        running = self.call("ui")
        if running["applied"] - idle["applied"] < 5:
            problems.append(f"time label not updating while running ({running['applied'] - idle['applied']} updates in 2 s)")
        self.snapshot("stopwatch", perf_after=perf["latest"])
        self.call("ui", semantic_operation("PRESS", generation, roles, "dial"))
        self.settle(0.5)
        stopped = self.call("ui")
        self.settle(1.5)
        still = self.call("ui")
        if still["applied"] - stopped["applied"] > 1:
            problems.append(f"updates continue after stop ({still['applied'] - stopped['applied']})")
        self.call("ui", semantic_operation("PRESS", generation, roles, "lap"))  # reset while stopped
        self.settle(0.5)
        reset = self.call("ui")
        elf_status = self.call("elf")
        if elf_status["error"] or reset["failed"] or reset["error"]:
            problems.append(f"application or UI error: {elf_status['error']!r} failed={reset['failed']}")
        self.stopwatch_generation = generation
        return (FAIL if problems else PASS), {
            "origin": "INJECTED (RP1 SELECT, RE1 UI PRESS by role), not touch", "roles": roles,
            "updates_running_2s": running["applied"] - idle["applied"],
            "updates_after_stop_1_5s": still["applied"] - stopped["applied"],
            "injected": reset["injected"], "problems": problems}

    def s_after_close(self):
        generation = getattr(self, "stopwatch_generation", None)
        nav = self.nav()
        if generation is None or nav["state"] != "ACTIVE" or nav["selected"] != STOPWATCH:
            raise Blocked("stopwatch is not the running application")
        perf = self.call("perf", "PERF")
        self.call("launcher", f"EXIT {generation}")
        nav = self.wait_for("Home after exit", self.nav, lambda n: n["state"] == "HOME", 15)
        elf_status = self.wait_for("idle runtime", lambda: self.call("elf"),
                                   lambda s: s["state"] == m8.ELF_IDLE and not s["busy"], 8)
        self.settle(self.args.settle)
        entry = self.snapshot("after_close", perf_after=perf["latest"])
        problems = []
        if entry["ui"]["widgets"] != 0 or entry["ui"]["outstanding"] != 0:
            problems.append("UI widgets or handles left after close")
        if elf_status["resources"][3] != 0:
            problems.append("application resources left after close")
        details = {"nav": nav["state"], "resources": elf_status["resources"], "problems": problems}
        if "home" in self.snapshots:
            details["heap_delta_vs_home"] = heap_delta(self.snapshots["home"]["heap"], entry["heap"])
        return (FAIL if problems else PASS), details

    def s_full_refresh(self):
        self.go_home()
        perf = self.call("perf", "PERF")
        result = self.device.session(lambda port: display.bench(port, self.args.bench_frames))
        self.snapshot("full_refresh", perf_after=perf["latest"])
        ok = result["frames"] == self.args.bench_frames and result["errors"] == 0
        return (PASS if ok else FAIL), {
            "bench": result, "note": "period spans GUI iteration ends, not panel scanout"}

    def s_littlefs(self):
        self.go_home()
        results = []
        problems = []
        for kib in FS_SIZES_KIB:
            expected = fs_pattern_sha256(kib)
            prep = self.call("fsbench", f"PREP {kib}")
            entry = {"kib": kib, "expected_sha256": expected, "prep": prep, "reads": []}
            results.append(entry)
            if prep["code"] != 0:
                problems.append(f"PREP {kib} refused ({prep['code']})")
                continue
            try:
                for _ in range(self.args.fs_repeats):
                    perf = self.call("perf", "PERF")
                    read = self.call("fsbench", f"READ {kib}")
                    _, traces = self.perf_traces(perf["latest"])
                    read["gui_window"] = gui_window(traces, perf["latest"])
                    entry["reads"].append(read)
                    if read["code"] != 0:
                        problems.append(f"READ {kib} failed ({read['code']})")
                    elif read["sha256"] != expected:
                        problems.append(f"READ {kib} SHA-256 mismatch")
                entry["health_after"] = {"system_worker_stack": self.call("safe", "HEALTH")["stacks"]["system_worker"]}
            finally:
                entry["del"] = self.call("fsbench", f"DEL {kib}")
                if entry["del"]["code"] != 0:
                    problems.append(f"DEL {kib} failed ({entry['del']['code']})")
        return (FAIL if problems else PASS), {"sizes": results, "problems": problems,
                                              "worker": "total_us is the time the system worker was busy"}

    def keep_screen_on(self):
        """The screen-on scenarios need the panel lit: RW1 SCREEN ON and a
        diagnostics-owned DISPLAY_AWAKE inhibitor (no screen-off on timeout)."""
        self.call("power", "SCREEN ON")
        status = self.wait_for("screen on", lambda: self.call("power"),
                               lambda s: s["state"] != "screen_off", 5)
        held = self.call("power", f"INHIBIT {DISPLAY_AWAKE} {SCREEN_HOLD_S}")
        self.screen_handle = held["handle"]
        self.report["context"]["screen_during_scenarios"] = {
            "state": status["state"], "inhibitor": "DISPLAY_AWAKE (RW1 INHIBIT 2)", "handle": held["handle"]}

    def release_screen(self):
        handle = getattr(self, "screen_handle", None)
        if handle is None:
            return
        self.screen_handle = None
        self.call("power", f"RELEASE {handle.upper()}")

    def s_light_sleep(self):
        if not self.args.light_sleep:
            raise Blocked("light sleep not allowed (--light-sleep)")
        self.go_home()
        if getattr(self, "screen_handle", None) is not None:
            # Light sleep starts from the ordinary screen-off state.
            self.release_screen()
            self.call("power", "SCREEN OFF")
            self.wait_for("screen off", lambda: self.call("power"),
                          lambda s: s["state"] == "screen_off", 5)
        before = self.call("power")
        health_before = self.observe_uptime("before light sleep")
        reply = self.call("power", f"SLEEP {self.args.sleep_seconds}")
        sent = self.now()
        lost_at = None
        back_at = None
        deadline = sent + self.args.sleep_seconds + 60
        status = None
        while self.now() < deadline:
            status = self.probe_quietly("power", "STATUS")
            if status is None and lost_at is None:
                lost_at = self.now() - sent
            if status is not None and status["phase"] == "idle" and status["state"] != "light_sleep" \
                    and status["sleeps"] != before["sleeps"]:
                back_at = self.now() - sent
                break
            self.sleep(0.5)
        if status is None or back_at is None:
            raise DeviceError("no idle power status after light sleep")
        health_after = self.observe_uptime("after light sleep")
        nav = self.settled_nav()
        problems = []
        if status["sleeps"] != before["sleeps"] + 1:
            problems.append(f"sleep not entered once (refusal {status['refusal']})")
        if health_after["uptime_us"] < health_before["uptime_us"]:
            problems.append("the board reset instead of waking")
        self.snapshot("after_light_sleep")
        return (FAIL if problems else PASS), {
            "before": before, "reply": reply, "after": status, "nav_after": nav["state"],
            "usb_lost_after_s": None if lost_at is None else round(lost_at, 2),
            "usb_back_after_s": round(back_at, 2),
            "usb_note": "USB loss and return are host observations, not the wake moment",
            "wake_time": PENDING, "problems": problems}

    SCENARIOS = ("home", "stopwatch", "after_close", "full_refresh", "littlefs", "light_sleep")

    def run_scenario(self, name):
        if self.halted:
            return self.record(name, BLOCKED, {"reason": f"run halted: {self.halted}"})
        faults_before = len(self.monitor.faults)
        try:
            status, details = getattr(self, "s_" + name)()
        except Blocked as error:
            return self.record(name, BLOCKED, {"reason": str(error)})
        except (DeviceError, TransportError, OSError, KeyError) as error:
            status, details = FAIL, {"error": f"{type(error).__name__}: {error}"}
        if len(self.monitor.faults) > faults_before:
            status = FAIL
            details["faults"] = self.monitor.faults[faults_before:]
            self.halted = "serial fault"
        if self.report["monitoring"]["unexpected_resets"]:
            status = FAIL
            self.halted = "unexpected reset"
        return self.record(name, status, details)


def gate(report):
    """Gate items of E0 (docs/roadmap.md) with their status in this run."""
    steps = {s["name"]: s["status"] for s in report["steps"]}
    snaps = report["snapshots"]
    scenarios = ("home", "stopwatch", "full_refresh", "after_close")

    def collected(key):
        missing = [s for s in scenarios if s not in snaps or key not in snaps[s]]
        return PASS if not missing else (BLOCKED if len(missing) == len(scenarios) else FAIL)

    items = [
        ("Heap internal/PSRAM current, minimum, largest block", collected("heap")),
        ("Stack margins", collected("stacks")),
        ("GUI/DMA times", collected("gui_window")),
        ("LittleFS free space", collected("littlefs")),
        ("Stopwatch start and close (INJECTED)", steps.get("stopwatch", BLOCKED)
         if steps.get("after_close", BLOCKED) == PASS else steps.get("after_close", BLOCKED)),
        ("LittleFS read + SHA-256 64/512/1024 KiB", steps.get("littlefs", BLOCKED)),
        ("Light sleep with expected USB loss, state after return", steps.get("light_sleep", BLOCKED)),
        ("Brightness, power source, charging, USB recorded",
         PASS if report["context"].get("power") and "error" not in report["context"]["power"] else BLOCKED),
    ]
    items += [(entry["needs"], PENDING) for entry in report["pending"]]
    return items


def outcome(report):
    statuses = [s["status"] for s in report["steps"]]
    if FAIL in statuses or report["monitoring"]["unexpected_resets"]:
        return FAIL
    if BLOCKED in statuses or not statuses:
        return BLOCKED
    return PASS


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True, choices=m8.BOARDS)
    parser.add_argument("--serial", help="USB serial number (MAC) of the board")
    parser.add_argument("--port", help="/dev/serial/by-id path of the board")
    parser.add_argument("--out", help="report directory (default build-ci/e0/<board>-<UTC>)")
    parser.add_argument("--ota", help="install this roundie_rep.bin first through RO1")
    parser.add_argument("--elf", help="local roundie_rep.elf to compare with the running image")
    parser.add_argument("--commit", default=None, help="source commit of the image (default: git HEAD)")
    parser.add_argument("--only", nargs="*", choices=Runner.SCENARIOS)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--bench-frames", type=int, default=120)
    parser.add_argument("--fs-repeats", type=int, default=3)
    parser.add_argument("--light-sleep", action="store_true", help="allow one timed light sleep")
    parser.add_argument("--sleep-seconds", type=int, default=20)
    args = parser.parse_args(argv)
    if args.commit is None:
        try:
            head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=HERE, capture_output=True, text=True, check=True)
            dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=HERE,
                                   capture_output=True, text=True, check=True)
            args.commit = head.stdout.strip() + ("-dirty" if dirty.stdout.strip() else "")
        except (OSError, subprocess.CalledProcessError):
            args.commit = "unknown"
    return args


def main(argv=None, device_factory=Device, by_id=m8.BY_ID, sleep=time.sleep, now=time.monotonic):
    args = parse_args(argv)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = Path(args.out or m8.ROOT / "build-ci" / "e0" / f"{args.board}-{stamp}")
    out.mkdir(parents=True, exist_ok=True)
    monitor = m8.Monitor(out / "serial.log")

    def terminate(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    runner = None
    try:
        try:
            path, serial = m8.discover(args.port, args.serial, by_id)
        except Blocked as error:
            report = {"runner": RUNNER_VERSION, "outcome": BLOCKED, "reason": str(error), "board": args.board}
            (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            print(f"BLOCKED: {error}")
            return 2
        runner = Runner(args, device_factory(path, monitor), monitor, out, sleep, now)
        if args.ota:
            try:
                runner.update(args.ota)
            except (Blocked, DeviceError, TransportError) as error:
                runner.report["ota"] = {"status": FAIL, "error": str(error)}
                runner.halted = f"OTA: {error}"
        problems = runner.context(serial)
        if problems:
            runner.halted = "; ".join(problems)
        runner.observe_uptime("start")
        if not runner.halted:
            try:
                runner.keep_screen_on()
            except (DeviceError, TransportError) as error:
                runner.report["context"]["screen_during_scenarios"] = {"error": str(error)}
        try:
            for name in args.only or Runner.SCENARIOS:
                runner.run_scenario(name)
        finally:
            try:
                runner.release_screen()
            except (DeviceError, TransportError) as error:
                runner.report["monitoring"]["notices"].append(f"inhibitor release: {error}")
        runner.report["gate"] = [{"item": item, "status": status} for item, status in gate(runner.report)]
        runner.report["outcome"] = outcome(runner.report)
        runner.report["finished"] = m8.utc_now()
        runner.write_report()
        print(f"{runner.report['outcome']}: {out / 'report.json'}")
        return {PASS: 0, FAIL: 1, BLOCKED: 2}[runner.report["outcome"]]
    except KeyboardInterrupt:
        if runner is not None:
            runner.report["outcome"] = "INTERRUPTED"
            runner.write_report()
        return 130
    finally:
        monitor.close()


if __name__ == "__main__":
    sys.exit(main())
