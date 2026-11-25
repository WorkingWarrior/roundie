#!/usr/bin/env python3
"""E0 stall, race and boot-history measurements (docs/baseline/e0-1.85b.md §10-§12).

Needs a development image with RP1 FSBENCH X*, RP1 ELFDELAY and RS1 HISTORY
(0.3.82-e0 or later; lowprio needs 0.3.84-e0, loggap 0.3.84-e0). Scenarios:

  history      RS1 HISTORY: the diagnostic ring of reset reasons (read only).
  stall_write  On Home: FSBENCH XWRITE 64/512/1024 KiB with chunk, yield and
               core variants; GUI loop and probe-task gaps while writing.
  stall_read   With the stopwatch running: FSBENCH XREAD 1024 KiB variants
               (chunk, yield, core) and the stopwatch label updates meanwhile.
  lowprio      With the stopwatch running: FSBENCH XREAD 1024 / XWRITE 512 in a
               helper below the GUI (L unpinned, L0 on core 0, no yield)
               against W 16384 y1; GUI and probe gaps, throughput and the
               system worker's reply time to RS1 STATUS and RP1 LIST during I/O.
  loggap       With the stopwatch running, rep_app at INFO and at WARN: the
               longest gap between application timer posts (RP1 TICKGAP)
               with the host port closed, open and read, and during XREAD.
  race         ELFDELAY holds a UI event between the executor taking it and
               on_event; EXIT inside that window. Counts RESULT views (the
               STOP/on_event race) against the same sequence without delay.

Same safety rules as run_e0.py: no Flash erase, no NVS, partition or eFuse
writes by this runner; the only LittleFS writes are the FSBENCH test file
(/storage/dev/e0-<kib>.bin, removed again) and, with --ota, one OTA image.
"""
import argparse
import json
import re
import secrets
import signal
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_e0 as e0  # noqa: E402
import run_e0_followup as followup  # noqa: E402
import run_m8 as m8  # noqa: E402
from run_m8 import BLOCKED, FAIL, PASS, Blocked  # noqa: E402
from roundie_time import DeviceError, TransportError, read_serial_reply, write_command  # noqa: E402

STALL_VERSION = 1
X_OPERATION = re.compile(r"X(READ|WRITE) (64|512|1024) ([0-9]{3,5}) ([0-9]{1,2}) (W|C0|C1|L|L0)")
# Nearly idle reference: 16 reads of 4 KiB, 100 ms (10 ticks) apart.
IDLE_REFERENCE = "XREAD 64 4096 10 W"
WRITE_VARIANTS = ((4096, 0, "W"), (4096, 1, "W"), (16384, 0, "W"), (4096, 0, "C0"), (4096, 0, "C1"))
# Priority model (§14): a helper below the GUI without yield, against the
# synchronous worker with 16 KiB chunks and one tick of yield.
LOW_VARIANTS = ((16384, 1, "W"), (4096, 0, "L"), (4096, 0, "L0"), (16384, 0, "L0"))
READ_VARIANTS = ((4096, 0, "W"), (4096, 1, "W"), (512, 0, "W"), (16384, 0, "W"), (16384, 1, "W"),
                 (32768, 0, "W"), (4096, 0, "C0"), (4096, 0, "C1"))


def xbench_request(port, operation, timeout=60.0):
    """RP1 FSBENCH X<READ|WRITE> <kib> <chunk> <yield> <W|C0|C1>."""
    match = X_OPERATION.fullmatch(operation)
    if match is None:
        raise TransportError("invalid FSBENCH X operation")
    identity = secrets.token_hex(4)
    prefix = f"!RP1 {identity} ".encode()
    write_command(port, f"!RP1 {identity} FSBENCH {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 1000)
    return parse_x_reply(line, prefix, "X" + match.group(1))


def parse_x_reply(line, prefix, kind=None):
    """A full FSBENCH X reply line; kind checks XREAD/XWRITE when given."""
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        result = json.loads(payload)
        if result["format"] != 1 or result["fsbench"] not in ("XREAD", "XWRITE"):
            raise ValueError("format")
        if kind is not None and result["fsbench"] != kind:
            raise ValueError("kind")
        for key in ("io_us", "sha_us", "total_us", "chunk_max_us", "migrations", "mmap_remain"):
            if type(result[key]) is not int or result[key] < 0:
                raise ValueError(key)
        if len(result["gui"]) != 3 or len(result["probes"]) != 4 or len(result["on_core"]) != 2:
            raise ValueError("arrays")
        result["code"] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed FSBENCH X reply: {error}") from error
    return result


def exchange(port, protocol, operation, timeout, limit=2000):
    """One addressed request; returns (code, payload bytes, seconds to reply)."""
    identity = secrets.token_hex(4)
    prefix = f"{protocol} {identity} ".encode()
    started = time.monotonic()
    write_command(port, f"{protocol} {identity} {operation}")
    line = read_serial_reply(port, prefix, started + timeout, limit)
    elapsed = time.monotonic() - started
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        return int(code), payload, elapsed, line, prefix
    except ValueError as error:
        raise TransportError(f"malformed reply to {operation}") from error


def low_session(port, operation, timeout=180.0, period=0.2, list_round=2):
    """Starts an L/L0 job, then every `period` s sends RS1 STATUS and XRESULT
    (and RP1 LIST once, in round `list_round`) until the result arrives. Each
    reply time is a system-worker response time during the I/O; the pause
    keeps the polling itself from loading the worker."""
    match = X_OPERATION.fullmatch(operation)
    if match is None or match.group(5) not in ("L", "L0"):
        raise TransportError("invalid low-priority FSBENCH X operation")
    code, payload, start_s, _, _ = exchange(port, "!RP1", f"FSBENCH {operation}", 4.0)
    if code != 0:
        return {"code": code, "start_s": start_s}
    if json.loads(payload).get("started") != 1:
        raise TransportError("FSBENCH X start not acknowledged")
    latency = {"STATUS": [], "LIST": [], "XRESULT": []}
    deadline = time.monotonic() + timeout
    rounds = 0
    while time.monotonic() < deadline:
        time.sleep(period)
        _, _, seconds, _, _ = exchange(port, "!RS1", "STATUS", 8.0)
        latency["STATUS"].append(seconds)
        if rounds == list_round:
            _, _, seconds, _, _ = exchange(port, "!RP1", "LIST", 8.0)
            latency["LIST"].append(seconds)
        rounds += 1
        code, _, seconds, line, prefix = exchange(port, "!RP1", "FSBENCH XRESULT", 4.0)
        latency["XRESULT"].append(seconds)
        if code == 259:
            continue
        if code != 0:
            raise TransportError(f"XRESULT failed ({code})")
        result = parse_x_reply(line, prefix, "X" + match.group(1))
        result["start_s"] = start_s
        result["worker_reply_s"] = latency_summary(latency)
        return result
    raise TransportError("low-priority FSBENCH X did not finish")


def tick_request(port, operation, timeout=4.0):
    """RP1 TICKGAP or RP1 LOGLEVEL <0..5>."""
    if re.fullmatch(r"TICKGAP|LOGLEVEL [0-5]", operation) is None:
        raise TransportError("invalid TICKGAP/LOGLEVEL operation")
    code, payload, _, _, _ = exchange(port, "!RP1", operation, timeout, 200)
    try:
        result = json.loads(payload)
        if result["format"] != 1:
            raise ValueError("format")
        if operation == "TICKGAP" and not (type(result["ticks"]) is int and type(result["gap_max_us"]) is int):
            raise ValueError("fields")
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed {operation} reply: {error}") from error
    result["code"] = code
    return result


def drain(port, seconds):
    """Keeps reading (and logging) the port for `seconds`."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        port.read(1)


def latency_summary(latency):
    return {name: {"n": len(values), "max": round(max(values), 4), "mean": round(sum(values) / len(values), 4)}
            for name, values in latency.items() if values}


def elfdelay_request(port, operation, timeout=4.0):
    """RP1 ELFDELAY <ms>."""
    if re.fullmatch(r"ELFDELAY [0-9]{1,4}", operation) is None:
        raise TransportError("invalid ELFDELAY operation")
    identity = secrets.token_hex(4)
    prefix = f"!RP1 {identity} ".encode()
    write_command(port, f"!RP1 {identity} {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 200)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        result = json.loads(payload)
        if result["format"] != 1 or type(result["elfdelay"]) is not int:
            raise ValueError("format")
        result["code"] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed ELFDELAY reply: {error}") from error
    return result


def history_request(port, operation="HISTORY", timeout=8.0):
    """RS1 HISTORY (read only)."""
    identity = secrets.token_hex(4)
    prefix = f"!RS1 {identity} ".encode()
    write_command(port, f"!RS1 {identity} HISTORY")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 600)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        history = json.loads(payload)["history"]
        if history["state"] not in ("unavailable", "ok", "malformed"):
            raise ValueError("state")
        for entry in history["entries"]:
            if len(entry) != 3 or not all(type(v) is int and v >= 0 for v in entry):
                raise ValueError("entry")
        history["code"] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed HISTORY reply: {error}") from error
    return history


e0.HANDLERS["xbench"] = lambda port, operation, timeout: xbench_request(port, operation, timeout or 60.0)
e0.HANDLERS["elfdelay"] = lambda port, operation, timeout: elfdelay_request(port, operation, timeout or 4.0)
e0.HANDLERS["tick"] = lambda port, operation, timeout: tick_request(port, operation, timeout or 4.0)
e0.HANDLERS["history"] = lambda port, operation, timeout: history_request(port, operation, timeout or 8.0)


def stall_row(operation, reply):
    """One measurement: GUI loop and probe gaps in ms, per probe core/priority."""
    probes = {f"c{core}p{priority}": round(gap / 1000, 1) for core, priority, _, gap in reply["probes"]}
    return {"op": operation, "code": reply["code"], "total_ms": round(reply["total_us"] / 1000, 1),
            "io_ms": round(reply["io_us"] / 1000, 1), "chunk_max_ms": round(reply["chunk_max_us"] / 1000, 1),
            "gui_iterations": reply["gui"][0], "gui_gap_ms": round(reply["gui"][1] / 1000, 1),
            "gui_late_ms": round(reply["gui"][2] / 1000, 1), "probe_gap_ms": probes,
            "on_core": reply["on_core"], "migrations": reply["migrations"],
            "mmap_remain": reply["mmap_remain"], "probes_armed": reply["probes_armed"]}


def race_verdict(nav):
    """Outcome of one EXIT: 'home' or the RESULT tuple shown instead."""
    if nav["state"] == "RESULT":
        return {"outcome": "result", "result": nav.get("result")}
    return {"outcome": nav["state"].lower()}


class StallRunner(followup.FollowupRunner):
    SCENARIOS = ("history", "stall_write", "stall_read", "lowprio", "loggap", "race")

    def xbench(self, operation, problems, expected=None):
        reply = self.call("xbench", operation)
        row = stall_row(operation, reply)
        if reply["code"] != 0:
            problems.append(f"{operation}: code {reply['code']}")
        elif expected is not None and reply["sha256"] != expected:
            problems.append(f"{operation}: SHA-256 mismatch")
        if not reply["probes_armed"]:
            problems.append(f"{operation}: probes not armed")
        return row

    def s_history(self):
        history = self.call("history")
        status = PASS if history["code"] == 0 and history["state"] == "ok" else FAIL
        return status, {"history": history}

    def s_stall_write(self):
        self.go_home()
        self.settle(2.0)
        problems = []
        rows = []
        # After a restart FSBENCH answers ESP_ERR_INVALID_STATE (259) until the
        # runtime admits work; wait for that instead of measuring refusals.
        self.wait_for("FSBENCH admission", lambda: self.call("xbench", "XWRITE 64 4096 0 W"),
                      lambda reply: reply["code"] != 259, 120, interval=2.0)
        try:
            rows.append(self.xbench(IDLE_REFERENCE, problems, e0.fs_pattern_sha256(64)))
            for kib in (64, 512):
                for chunk, yield_ticks, core in WRITE_VARIANTS:
                    for _ in range(self.args.repeats):
                        rows.append(self.xbench(f"XWRITE {kib} {chunk} {yield_ticks} {core}", problems))
                        self.settle(0.5)
                rows.append(self.xbench(f"XREAD {kib} 4096 0 W", problems, e0.fs_pattern_sha256(kib)))
                self.call("fsbench", f"DEL {kib}")
            rows.append(self.xbench("XWRITE 1024 4096 0 W", problems))
            rows.append(self.xbench("XREAD 1024 4096 0 W", problems, e0.fs_pattern_sha256(1024)))
        finally:
            for kib in (64, 512, 1024):
                deleted = self.call("fsbench", f"DEL {kib}")
                if deleted["code"] not in (0, 261):  # 261 = ESP_ERR_NOT_FOUND
                    problems.append(f"DEL {kib} failed ({deleted['code']})")
        return (FAIL if problems else PASS), {"rows": rows, "problems": problems}

    def s_stall_read(self):
        self.go_home()
        problems = []
        details = {}
        expected = e0.fs_pattern_sha256(1024)
        prep = self.call("xbench", "XWRITE 1024 4096 0 W")
        if prep["code"] != 0:
            raise Blocked(f"XWRITE 1024 refused ({prep['code']})")
        generation = None
        rows = []
        try:
            generation, roles = self.open_stopwatch()
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.settle(1.0)
            # Nearly idle: 32 reads of 32 KiB, 100 ms apart.
            details["idle_reference"] = self.xbench("XREAD 1024 32768 10 W", problems, expected)
            for _ in range(self.args.repeats):
                for chunk, yield_ticks, core in READ_VARIANTS:
                    ui_before = self.call("ui")
                    row = self.xbench(f"XREAD 1024 {chunk} {yield_ticks} {core}", problems, expected)
                    ui_after = self.call("ui")
                    row["label_updates"] = ui_after["applied"] - ui_before["applied"]
                    rows.append(row)
                    self.settle(0.5)
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.close_stopwatch(generation)
            generation = None
        finally:
            if generation is not None:
                self.close_stopwatch(generation)
            deleted = self.call("fsbench", "DEL 1024")
            if deleted["code"] != 0:
                problems.append(f"DEL 1024 failed ({deleted['code']})")
        details.update(rows=rows, problems=problems,
                       note="label_updates spans the two RE1 UI reads around each XREAD reply")
        return (FAIL if problems else PASS), details

    def idle_worker_latency(self, repeats=5):
        latency = {"STATUS": [], "LIST": []}
        for _ in range(repeats):
            for name in latency:
                protocol = "!RS1" if name == "STATUS" else "!RP1"
                _, _, seconds, _, _ = self.device.session(lambda port: exchange(port, protocol, name, 8.0))
                latency[name].append(seconds)
        return latency_summary(latency)

    def low_row(self, operation, problems, expected):
        """One L/L0 run, or the W baseline with RS1 STATUS sent 20 ms after it."""
        ui_before = self.call("ui")
        if operation.endswith(" W"):
            timing, lines = self.device.session(lambda port: followup.timed_pair(
                port, ("!RP1", f"FSBENCH {operation}"), ("!RS1", "STATUS"), 0.02, timeout=60.0))
            prefix = lines[0].split(b" ", 2)
            reply = parse_x_reply(lines[0], b" ".join(prefix[:2]) + b" ")
            reply["worker_reply_s"] = {"STATUS": {"n": 1, "max": round(timing["second_reply_s"], 4),
                                                  "mean": round(timing["second_reply_s"], 4)}}
        else:
            reply = self.device.session(lambda port: low_session(port, operation, list_round=self.args.list_round))
            if reply["code"] != 0:
                problems.append(f"{operation}: start refused ({reply['code']})")
                return {"op": operation, "code": reply["code"]}
        ui_after = self.call("ui")
        row = stall_row(operation, reply)
        kib = int(operation.split()[1])
        row["kib_per_s"] = round(kib / (reply["total_us"] / 1e6), 1)
        row["worker_reply_s"] = reply["worker_reply_s"]
        row["label_updates"] = ui_after["applied"] - ui_before["applied"]
        if reply["code"] != 0:
            problems.append(f"{operation}: code {reply['code']}")
        elif expected is not None and reply["sha256"] != expected:
            problems.append(f"{operation}: SHA-256 mismatch")
        if not reply["probes_armed"]:
            problems.append(f"{operation}: probes not armed")
        return row

    def s_lowprio(self):
        self.go_home()
        self.settle(2.0)
        problems = []
        details = {}
        self.wait_for("FSBENCH admission", lambda: self.call("xbench", "XWRITE 64 4096 0 W"),
                      lambda reply: reply["code"] != 259, 120, interval=2.0)
        self.call("fsbench", "DEL 64")
        prep = self.call("xbench", "XWRITE 1024 4096 0 W")
        if prep["code"] != 0:
            raise Blocked(f"XWRITE 1024 refused ({prep['code']})")
        expected = e0.fs_pattern_sha256(1024)
        generation = None
        reads = []
        writes = []
        try:
            details["idle_worker_reply_s"] = self.idle_worker_latency()
            generation, roles = self.open_stopwatch()
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.settle(1.0)
            details["running_worker_reply_s"] = self.idle_worker_latency()
            for _ in range(self.args.repeats):
                for chunk, yield_ticks, where in LOW_VARIANTS:
                    reads.append(self.low_row(f"XREAD 1024 {chunk} {yield_ticks} {where}", problems, expected))
                    self.settle(0.5)
            # Writes need an idle runtime (as PREP): stop and close the stopwatch first.
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.close_stopwatch(generation)
            generation = None
            self.go_home()
            self.settle(1.0)
            for _ in range(self.args.repeats):
                for chunk, yield_ticks, where in LOW_VARIANTS:
                    writes.append(self.low_row(f"XWRITE 512 {chunk} {yield_ticks} {where}", problems, None))
                    self.settle(0.5)
            check = self.xbench("XREAD 512 4096 0 W", problems, e0.fs_pattern_sha256(512))
            details["write_check"] = check
        finally:
            if generation is not None:
                self.close_stopwatch(generation)
            for kib in (512, 1024):
                deleted = self.call("fsbench", f"DEL {kib}")
                if deleted["code"] not in (0, 261):
                    problems.append(f"DEL {kib} failed ({deleted['code']})")
        details.update(reads=reads, writes=writes, problems=problems,
                       note=("worker_reply_s: request-to-reply seconds of system-worker commands sent while the "
                             "I/O runs (L/L0: polled until XRESULT; W: RS1 STATUS 20 ms after the command)"))
        return (FAIL if problems else PASS), details

    def tick_window(self, condition, seconds=3.0):
        """Longest application timer gap while the host does `condition`."""
        if condition == "closed":
            self.call("tick", "TICKGAP")
            time.sleep(seconds)
            reply = self.call("tick", "TICKGAP")
        elif condition == "open":
            def run(port):
                tick_request(port, "TICKGAP")
                drain(port, seconds)
                return tick_request(port, "TICKGAP")
            reply = self.device.session(run)
        else:
            self.call("tick", "TICKGAP")
            read = self.call("xbench", "XREAD 1024 4096 0 W")
            reply = self.call("tick", "TICKGAP")
            reply["read_ms"] = round(read["total_us"] / 1000, 1)
        return {"condition": condition, "ticks": reply["ticks"], "gap_max_ms": round(reply["gap_max_us"] / 1000, 1),
                **({"read_ms": reply["read_ms"]} if "read_ms" in reply else {})}

    def s_loggap(self):
        self.go_home()
        problems = []
        prep = self.call("xbench", "XWRITE 1024 4096 0 W")
        if prep["code"] != 0:
            raise Blocked(f"XWRITE 1024 refused ({prep['code']})")
        generation = None
        rows = []
        try:
            generation, roles = self.open_stopwatch()
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.settle(1.0)
            for _ in range(self.args.repeats):
                for level in (3, 2):
                    if self.call("tick", f"LOGLEVEL {level}")["code"] != 0:
                        problems.append(f"LOGLEVEL {level} refused")
                    for condition in ("closed", "open", "xread"):
                        row = self.tick_window(condition)
                        row["loglevel"] = level
                        rows.append(row)
            self.call("tick", "LOGLEVEL 3")
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.close_stopwatch(generation)
            generation = None
        finally:
            self.call("tick", "LOGLEVEL 3")
            if generation is not None:
                self.close_stopwatch(generation)
            deleted = self.call("fsbench", "DEL 1024")
            if deleted["code"] != 0:
                problems.append(f"DEL 1024 failed ({deleted['code']})")
        return (FAIL if problems else PASS), {"rows": rows, "problems": problems,
                                             "note": "loglevel 3 = INFO (rep_app lines on), 2 = WARN (off); "
                                                     "timer period 100 ms"}

    def race_attempt(self, delay_ms):
        generation, roles = self.open_stopwatch()
        # Lap is accepted only while the stopwatch runs.
        self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
        self.settle(0.5)
        self.call("elfdelay", f"ELFDELAY {delay_ms}")
        try:
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "lap"))
            time.sleep(self.args.exit_after)
            self.call("launcher", f"EXIT {generation}")
            # The launcher decides between Home and RESULT once the unload ends.
            self.wait_for("idle runtime", lambda: self.call("elf"),
                          lambda s: s["state"] == m8.ELF_IDLE and not s["busy"], 15)
            nav = self.settled_nav()
        finally:
            self.call("elfdelay", "ELFDELAY 0")
        verdict = race_verdict(nav)
        self.go_home()
        return verdict

    def s_race(self):
        self.go_home()
        series = {}
        for delay_ms in (self.args.race_delay, 0):
            series[str(delay_ms)] = [self.race_attempt(delay_ms) for _ in range(self.args.race_repeats)]
            self.write_report()
        delayed = series[str(self.args.race_delay)]
        reproduced = sum(1 for v in delayed if v["outcome"] == "result")
        control = sum(1 for v in series["0"] if v["outcome"] == "result")
        problems = []
        if reproduced != len(delayed):
            problems.append(f"race reproduced {reproduced}/{len(delayed)} with delay")
        return (FAIL if problems else PASS), {
            "delay_ms": self.args.race_delay, "exit_after_s": self.args.exit_after, "series": series,
            "reproduced": [reproduced, len(delayed)], "control_results": [control, len(series["0"])],
            "problems": problems}


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True, choices=m8.BOARDS)
    parser.add_argument("--serial")
    parser.add_argument("--port")
    parser.add_argument("--out")
    parser.add_argument("--ota", help="install this roundie_rep.bin first through RO1")
    parser.add_argument("--elf")
    parser.add_argument("--commit", default=None)
    parser.add_argument("--only", nargs="*", choices=StallRunner.SCENARIOS)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--race-delay", type=int, default=300)
    parser.add_argument("--race-repeats", type=int, default=10)
    parser.add_argument("--exit-after", type=float, default=0.05)
    parser.add_argument("--list-round", type=int, default=2,
                        help="lowprio: poll round that also sends RP1 LIST (-1: never)")
    args = parser.parse_args(argv)
    if args.commit is None:
        args.commit = e0.parse_args(["--board", args.board]).commit
    return args


def main(argv=None, device_factory=e0.Device, by_id=m8.BY_ID):
    args = parse_args(argv)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = Path(args.out or m8.ROOT / "build-ci" / "e0" / f"stall-{args.board}-{stamp}")
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
            (out / "report.json").write_text(json.dumps(
                {"stall": STALL_VERSION, "outcome": BLOCKED, "reason": str(error)}, indent=2) + "\n")
            print(f"BLOCKED: {error}")
            return 2
        runner = StallRunner(args, device_factory(path, monitor), monitor, out)
        runner.report["stall"] = STALL_VERSION
        runner.report["pending"] = []
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
            runner.keep_screen_on()
        try:
            for name in args.only or StallRunner.SCENARIOS:
                runner.run_scenario(name)
        finally:
            try:
                runner.release_screen()
            except (DeviceError, TransportError) as error:
                runner.report["monitoring"]["notices"].append(f"inhibitor release: {error}")
        runner.report["outcome"] = e0.outcome(runner.report)
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
