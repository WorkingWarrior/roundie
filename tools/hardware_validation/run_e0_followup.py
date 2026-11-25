#!/usr/bin/env python3
"""E0 follow-up measurements on one Roundie board (docs/baseline/e0-summary.md).

Reuses the E0 runner (run_e0.py) and adds four scenarios:

  memprobe     RS1 HEALTH heap figures against a controlled RP1 MEMPROBE
               allocation of known size (internal and PSRAM), then freed.
  cycles       N launch/close cycles of the stopwatch with heap after each.
  worst_stack  GUI stack minimum after full refresh on the running stopwatch,
               navigation and full refresh on Home and the list.
  fsbench_gui  RP1 FSBENCH READ 1024 while the stopwatch runs: GUI iterations
               and the reply delay of a system-worker command, against the
               same measurements without FSBENCH.

Same safety rules as run_e0.py: no Flash erase, no NVS, partition or eFuse
writes; the only Flash writes are the FSBENCH test file (removed again) and,
with --ota, one OTA image.
"""
import argparse
import json
import secrets
import signal
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_e0 as e0  # noqa: E402
import run_m8 as m8  # noqa: E402
from run_m8 import BLOCKED, FAIL, PASS, Blocked  # noqa: E402
import roundie_display as display  # noqa: E402
from roundie_time import DeviceError, TransportError, read_serial_reply, write_command  # noqa: E402

FOLLOWUP_VERSION = 1
# TLSF block header plus alignment; heap poisoning is off in the rep profile.
ALLOC_OVERHEAD_MAX = 64
# Other tasks may allocate between two HEALTH reads; tolerated drift.
HEAP_NOISE = 256
PROBES = (("INTERNAL", 1024), ("INTERNAL", 16384), ("PSRAM", 4096), ("PSRAM", 131072))
POOL = {"INTERNAL": "internal_free", "PSRAM": "psram_free"}
GUI_STACK_RISK = 1024


def memprobe_request(port, operation, timeout=4.0):
    """RP1 MEMPROBE [ALLOC INTERNAL|PSRAM <bytes> | FREE]."""
    identity = secrets.token_hex(4)
    prefix = f"!RP1 {identity} ".encode()
    write_command(port, f"!RP1 {identity} {operation}")
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 400)
    try:
        code, payload = line[len(prefix):].split(b" ", 1)
        result = json.loads(payload)
        if result["format"] != 1 or "memprobe" not in result:
            raise ValueError("format")
        result["code"] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f"malformed MEMPROBE reply: {error}") from error
    return result


e0.HANDLERS["memprobe"] = lambda port, operation, timeout: memprobe_request(port, operation, timeout or 4.0)


def probe_verdict(pool, size, before, held, after):
    """Checks one controlled allocation against three HEALTH readings."""
    key = POOL[pool]
    other = POOL["PSRAM" if pool == "INTERNAL" else "INTERNAL"]
    seen = before[key] - held[key]
    back = after[key] - before[key]
    problems = []
    if not size <= seen <= size + ALLOC_OVERHEAD_MAX:
        problems.append(f"{key} dropped by {seen}, expected {size}..{size + ALLOC_OVERHEAD_MAX}")
    if abs(back) > HEAP_NOISE:
        problems.append(f"{key} after FREE differs by {back}")
    if abs(before[other] - held[other]) > HEAP_NOISE:
        problems.append(f"{other} moved by {before[other] - held[other]} during a {pool} allocation")
    return {"pool": pool, "bytes": size, "seen_drop": seen, "overhead": seen - size,
            "after_free": back, "other_pool_drop": before[other] - held[other], "problems": problems}


def linear_slope(values):
    """Least-squares slope per step; None for fewer than two values."""
    n = len(values)
    if n < 2:
        return None
    mean_x = (n - 1) / 2
    mean_y = sum(values) / n
    num = sum((i - mean_x) * (v - mean_y) for i, v in enumerate(values))
    den = sum((i - mean_x) ** 2 for i in range(n))
    return num / den


def classify_cycles(baseline, after_each):
    """'arena' when the only lasting drop comes with the first cycle and the
    later cycles are flat; 'leak' when later cycles keep losing memory."""
    result = {}
    for key in ("psram_free", "internal_free"):
        series = [a[key] for a in after_each]
        later = series[1:]
        result[key] = {
            "first_cycle_drop": baseline[key] - series[0] if series else None,
            "later_min": min(later) if later else None, "later_max": max(later) if later else None,
            "later_vs_first": (later[-1] - series[0]) if later else None,
            "slope_per_cycle": linear_slope(later),
        }
    psram = result["psram_free"]
    internal = result["internal_free"]
    steady = all(r["later_vs_first"] is not None and abs(r["later_vs_first"]) <= HEAP_NOISE
                 and abs(r["slope_per_cycle"] or 0) < 16 for r in (psram, internal))
    result["class"] = "arena" if steady else "leak"
    return result


def longest_gap(traces):
    """Longest time between consecutive retained GUI iterations (µs)."""
    times = [t["at_us"] for t in traces]
    gaps = [b - a for a, b in zip(times, times[1:])]
    return max(gaps) if gaps else None


def frame_stats(traces):
    """Period between flushing GUI iterations and their handler/render times (µs)."""
    flushing = [t for t in traces if t["flushes"] > 0]
    stats = {"iterations": len(traces), "flushing": len(flushing)}
    periods = [b["at_us"] - a["at_us"] for a, b in zip(flushing, flushing[1:])]
    for name, values in (("period_us", periods), ("handler_us", [t["handler_us"] for t in flushing]),
                         ("render_us", [t["render_us"] for t in flushing]),
                         ("dma_us", [t["dma_us"] for t in flushing])):
        if values:
            stats[name] = {"max": max(values), "mean": round(sum(values) / len(values), 1)}
    return stats


def timed_pair(port, first, second, gap_s, timeout=10.0):
    """Writes two commands gap_s apart and timestamps both replies.
    first/second are (protocol prefix, operation). Returns (times, lines)."""
    ids = [secrets.token_hex(4), secrets.token_hex(4)]
    prefixes = [f"{p} {i} ".encode() for (p, _), i in zip((first, second), ids)]
    started = time.monotonic()
    write_command(port, f"{first[0]} {ids[0]} {first[1]}")
    time.sleep(gap_s)
    second_sent = time.monotonic()
    write_command(port, f"{second[0]} {ids[1]} {second[1]}")
    times = [None, None]
    lines = [None, None]
    line = bytearray()
    deadline = started + timeout
    while time.monotonic() < deadline and None in times:
        byte = port.read(1)
        if not byte:
            continue
        line.extend(byte)
        if byte == b"\n":
            for i, prefix in enumerate(prefixes):
                if bytes(line).startswith(prefix):
                    times[i] = time.monotonic()
                    lines[i] = bytes(line)
            line.clear()
    if None in times:
        raise TransportError("reply of a timed pair missing")
    return {"first_reply_s": times[0] - started, "second_sent_s": second_sent - started,
            "second_reply_s": times[1] - second_sent}, lines


class FollowupRunner(e0.Runner):
    SCENARIOS = ("memprobe", "cycles", "worst_stack", "fsbench_gui")

    def health_heap(self, label):
        health = self.observe_uptime(label)
        return e0.heap_figures(health), health

    # ---- 1. memory probe ----------------------------------------------
    def s_memprobe(self):
        self.go_home()
        status = self.call("memprobe", "MEMPROBE")
        if status["code"] != 0 or status["held"] != 0:
            raise Blocked(f"MEMPROBE not idle: {status}")
        results = []
        for pool, size in PROBES:
            before, _ = self.health_heap(f"memprobe {pool} {size} before")
            alloc = self.call("memprobe", f"MEMPROBE ALLOC {pool} {size}")
            if alloc["code"] != 0:
                results.append({"pool": pool, "bytes": size, "problems": [f"ALLOC refused {alloc['code']}"]})
                continue
            held, _ = self.health_heap(f"memprobe {pool} {size} held")
            freed = self.call("memprobe", "MEMPROBE FREE")
            after, _ = self.health_heap(f"memprobe {pool} {size} after")
            verdict = probe_verdict(pool, size, before, held, after)
            key = POOL[pool]
            verdict["device_drop"] = alloc[key.replace("_free", "_before")] - alloc[key.replace("_free", "_after")]
            if freed["code"] != 0:
                verdict["problems"].append(f"FREE refused {freed['code']}")
            verdict["health"] = {"before": before, "held": held, "after": after}
            results.append(verdict)
        problems = [p for r in results for p in r["problems"]]
        return (FAIL if problems else PASS), {"probes": results, "problems": problems,
                                              "tolerance": {"overhead_max": ALLOC_OVERHEAD_MAX, "noise": HEAP_NOISE}}

    # ---- 2. launch/close cycles ---------------------------------------
    def open_stopwatch(self):
        self.go_home()
        nav = self.open_list()
        self.call("launcher", f"SELECT {nav['revision']} {e0.STOPWATCH}")
        nav = self.wait_for("stopwatch active", self.nav,
                            lambda n: n["state"] == "ACTIVE" and n["selected"] == e0.STOPWATCH, 15)
        generation = nav["generation"]
        ui_status = self.wait_for("stopwatch widgets", lambda: self.call("ui"),
                                  lambda s: s["generation"] == generation and s["widgets"] == 5
                                  and s["pending"] == 0, 6)
        return generation, e0.stopwatch_roles(ui_status, generation)

    def close_stopwatch(self, generation):
        self.call("launcher", f"EXIT {generation}")
        self.wait_for("Home after exit", self.nav, lambda n: n["state"] == "HOME", 15)
        return self.wait_for("idle runtime", lambda: self.call("elf"),
                             lambda s: s["state"] == m8.ELF_IDLE and not s["busy"], 8)

    def s_cycles(self):
        self.go_home()
        self.settle(2.0)
        baseline, health = self.health_heap("cycles baseline")
        tasks_before = health["tasks"]
        after_each = []
        problems = []
        for cycle in range(1, self.args.cycles + 1):
            generation, roles = self.open_stopwatch()
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.settle(1.0)
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            elf_status = self.close_stopwatch(generation)
            self.settle(1.0)
            heap, health = self.health_heap(f"cycle {cycle}")
            heap.update(cycle=cycle, tasks=health["tasks"], resources=elf_status["resources"])
            after_each.append(heap)
            if elf_status["resources"][3] != 0:
                problems.append(f"cycle {cycle}: resources left {elf_status['resources']}")
            self.write_report()
        verdict = classify_cycles(baseline, after_each)
        return (FAIL if problems else PASS), {
            "baseline": baseline, "tasks_before": tasks_before, "after_each": after_each,
            "classification": verdict, "problems": problems}

    # ---- 4. worst-case GUI stack --------------------------------------
    def bench_with_traces(self, label):
        perf = self.call("perf", "PERF")
        result = self.device.session(lambda port: display.bench(port, self.args.bench_frames))
        _, traces = self.perf_traces(perf["latest"])
        return {"label": label, "bench": result, "frames": frame_stats(traces), "traces": traces}

    def s_worst_stack(self):
        stacks = []
        before, health = self.health_heap("worst_stack start")
        stacks.append(("start", health["stacks"]["main"]))
        benches = []
        generation, roles = self.open_stopwatch()
        self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
        benches.append(self.bench_with_traces("stopwatch running"))
        stacks.append(("after bench on running stopwatch", self.call("safe", "HEALTH")["stacks"]["main"]))
        self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
        self.call("ui", e0.semantic_operation("PRESS", generation, roles, "lap"))
        # Let the lap event finish before EXIT: STOP closes UI admission at once
        # (components/roundie_elf/manager.c:854), and a callback still running
        # then has ui_update rejected (stopwatch -24, launcher RESULT).
        self.settle(1.0)
        self.close_stopwatch(generation)
        self.open_list()
        benches.append(self.bench_with_traces("launcher list"))
        self.call("launcher", "HOME")
        self.wait_for("Home", self.nav, lambda n: n["state"] == "HOME", 10)
        benches.append(self.bench_with_traces("Home"))
        health = self.call("safe", "HEALTH")
        stacks.append(("after navigation and benches", health["stacks"]["main"]))
        perf = self.call("perf", "PERF")
        minimum = min(value for _, value in stacks)
        problems = [f"bench {b['label']}: {b['bench']['errors']} errors" for b in benches if b["bench"]["errors"]]
        return (FAIL if problems else PASS), {
            "main_stack_free": stacks, "minimum": minimum, "perf_stack_min": perf["summary"]["stack_min"],
            "bench_stack_min": {b["label"]: b["bench"].get("stack_min") for b in benches},
            "risk_below_bytes": GUI_STACK_RISK, "risk": minimum < GUI_STACK_RISK,
            "benches": benches, "problems": problems,
            "note": "high-water marks are minima since boot, not since the start of the scenario"}

    # ---- 5. FSBENCH with the GUI active -------------------------------
    def window(self, seconds):
        perf = self.call("perf", "PERF")
        ui_before = self.call("ui")
        self.settle(seconds)
        ui_after = self.call("ui")
        _, traces = self.perf_traces(perf["latest"])
        return {"seconds": seconds, "frames": frame_stats(traces),
                "label_updates": ui_after["applied"] - ui_before["applied"]}

    def worker_latency(self, repeats):
        """RS1 STATUS round trips through the system worker, idle."""
        values = []
        for _ in range(repeats):
            started = time.monotonic()
            self.call("safe", "STATUS")
            values.append(time.monotonic() - started)
        return {"max_s": max(values), "mean_s": sum(values) / len(values), "repeats": repeats}

    def s_fsbench_gui(self):
        self.go_home()
        prep = self.call("fsbench", "PREP 1024")
        if prep["code"] != 0:
            raise Blocked(f"PREP 1024 refused ({prep['code']})")
        expected = e0.fs_pattern_sha256(1024)
        problems = []
        details = {"prep": prep}
        generation = None
        try:
            generation, roles = self.open_stopwatch()
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.settle(1.0)
            details["baseline_window"] = self.window(2.0)
            details["baseline_latency"] = self.worker_latency(5)
            pairs = []
            for _ in range(self.args.fs_repeats):
                perf = self.call("perf", "PERF")
                ui_before = self.call("ui")
                timing, lines = self.device.session(lambda port: timed_pair(
                    port, ("!RP1", "FSBENCH READ 1024"), ("!RS1", "STATUS"), 0.02))
                ui_after = self.call("ui")
                # From the last iteration before READ, so the first gap spans it.
                _, traces = self.perf_traces(max(perf["latest"] - 1, 0))
                read = json.loads(lines[0].split(b" ", 3)[3])
                code = int(lines[0].split(b" ", 3)[2])
                if code != 0 or read["sha256"] != expected:
                    problems.append(f"READ 1024 code {code} or SHA-256 mismatch")
                pairs.append({"timing": timing, "read_total_us": read["total_us"], "read_us": read["read_us"],
                              "sha_us": read["sha_us"], "chunk_max_us": read["chunk_max_us"],
                              "frames": frame_stats(traces), "longest_gui_gap_us": longest_gap(traces),
                              "iterations_at_us": [t["at_us"] for t in traces],
                              "label_updates": ui_after["applied"] - ui_before["applied"]})
                self.settle(1.0)
            details["with_fsbench"] = pairs
            self.call("ui", e0.semantic_operation("PRESS", generation, roles, "dial"))
            self.close_stopwatch(generation)
            generation = None
        finally:
            if generation is not None:
                self.close_stopwatch(generation)
            details["del"] = self.call("fsbench", "DEL 1024")
            if details["del"]["code"] != 0:
                problems.append(f"DEL 1024 failed ({details['del']['code']})")
        details["problems"] = problems
        details["note"] = ("second_reply_s is the time from sending RS1 STATUS 20 ms after FSBENCH READ "
                           "to its reply; both are served by the system worker")
        return (FAIL if problems else PASS), details


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", required=True, choices=m8.BOARDS)
    parser.add_argument("--serial")
    parser.add_argument("--port")
    parser.add_argument("--out")
    parser.add_argument("--ota", help="install this roundie_rep.bin first through RO1")
    parser.add_argument("--elf")
    parser.add_argument("--commit", default=None)
    parser.add_argument("--only", nargs="*", choices=FollowupRunner.SCENARIOS)
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--settle", type=float, default=5.0)
    parser.add_argument("--bench-frames", type=int, default=120)
    parser.add_argument("--fs-repeats", type=int, default=3)
    args = parser.parse_args(argv)
    if args.commit is None:
        args.commit = e0.parse_args(["--board", args.board]).commit
    return args


def main(argv=None, device_factory=e0.Device, by_id=m8.BY_ID):
    args = parse_args(argv)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out = Path(args.out or m8.ROOT / "build-ci" / "e0" / f"followup-{args.board}-{stamp}")
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
                {"followup": FOLLOWUP_VERSION, "outcome": BLOCKED, "reason": str(error)}, indent=2) + "\n")
            print(f"BLOCKED: {error}")
            return 2
        runner = FollowupRunner(args, device_factory(path, monitor), monitor, out)
        runner.report["followup"] = FOLLOWUP_VERSION
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
            for name in args.only or FollowupRunner.SCENARIOS:
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
