#!/usr/bin/env python3
"""M8.1 unattended endurance runner for one Roundie board over USB.

Extends the M8 runner (run_m8.py): same discovery, preflight, serial
monitor, reset detection and restore. No touch, button, cable or manual
reset is needed. Levels:

  smoke   a few lifecycle and UI rounds, recovery, a short idle with RTC edges
  stress  --cycles REP lifecycle cycles, --ui-rounds round UI rounds, recovery
  soak    --hours of idle with the home application, periodic telemetry,
          RTC second-edge captures and lifecycle batches

The runner never writes the RTC, Flash partitions, NVS or eFuses. It writes
one ELF to the development staging file (/storage/dev) once per run, and the
bad-package check writes one more. Installed packages are started through the
launcher, so they cause no Flash writes.

Outputs in --out: report.json, report.md, telemetry.jsonl, serial.log.
Stop it with SIGTERM or Ctrl-C; the partial report is kept.
"""
import argparse
import json
import math
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_m8 as m8  # noqa: E402
from run_m8 import (BLOCKED, ELF_IDLE, FAIL, HOME, PASS, Blocked, DeviceError,  # noqa: E402
                    TransportError, fnv1a, leak_check, utc_now)

RUNNER_VERSION = 1
PACKAGES = {"hello": "org.roundie.hello", "ui": "org.roundie.ui", "round": "org.roundie.round",
            "watch": "org.roundie.watch", "stopwatch": "org.roundie.stopwatch"}
LIFECYCLE = ("hello", "event", "ui", "round", "watch", "stopwatch")
COLORS = ("Red", "Green", "Blue", "Amber")
LEVEL_MIN = 0
LEVEL_MAX = 100
LIST_SETTLE_S = 2.0
EDGE_LEAD_S = 0.15
EDGE_LIMIT_S = 1.4
HOST_STEP_NS = 5_000_000
HEALTH_KEYS = ("uptime_us", "internal_free", "internal_min", "internal_largest", "psram_free",
               "psram_min", "psram_largest", "alloc_failed", "alloc_failed_size", "alloc_failed_caps",
               "tasks", "stacks")
ELF_KEYS = ("state", "instance", "completed", "events", "users", "ui_events", "rejected", "dropped",
            "over_budget", "error", "resources", "latency_max_us", "after", "load_us", "unload_us")
UI_KEYS = ("generation", "widgets", "pending", "outstanding", "lvgl_bytes", "lvgl_peak", "injected",
           "delivered", "input_accepted", "input_dropped", "input_rejected", "failed", "error",
           "flush_errors", "input_latency_max_us")
# Memory series checked for a steady decline in one fixed device state, with
# the tolerance in bytes a decline must exceed (run_m8.leak_check).
LEAK_TOLERANCE = {"internal_free": 2048, "internal_largest": 4096, "psram_free": 8192, "lvgl_bytes": 256}

# PASS/FAIL criteria, fixed before any run and copied into every report.
CRITERIA = (
    ("resets", "0 unexpected resets (uptime never drops outside a planned restart)"),
    ("faults", "0 serial panics, watchdog reports or asserts"),
    ("integrity", "0 REP/UI integrity problems: every cycle unloads cleanly, no application error, "
                  "no executor rejection, drop or over-budget callback outside deliberate invalid input"),
    ("delivery", "every spaced injected input is delivered; in bursts accepted = delivered + stale-rejected"),
    ("allocation", "0 failed heap allocations (RS1 HEALTH alloc_failed does not grow)"),
    ("tasks", "task count shows no irreversible growth in the same device state"),
    ("memory", "no steady decline (leak_check) of free internal heap, largest block, PSRAM or LVGL bytes "
               "in the same device state after the warm-up"),
    ("communication", "every communication loss ends in a reconnect within --reconnect-timeout"),
)


# ---- pure analysis --------------------------------------------------------

def percentiles(values):
    """Nearest-rank statistics; median is the 50th percentile by nearest rank."""
    data = sorted(values)
    if not data:
        return {"count": 0}

    def rank(percent):
        return data[max(0, math.ceil(percent / 100 * len(data)) - 1)]

    return {"count": len(data), "min": data[0], "median": rank(50), "p95": rank(95),
            "p99": rank(99), "max": data[-1]}


def slope_per_hour(times_s, values):
    """Least-squares slope in units per hour, or None without a time spread."""
    count = len(values)
    if count < 2:
        return None
    mean_t = sum(times_s) / count
    mean_v = sum(values) / count
    spread = sum((t - mean_t) ** 2 for t in times_s)
    if spread == 0:
        return None
    return sum((t - mean_t) * (v - mean_v) for t, v in zip(times_s, values)) / spread * 3600


def task_stability(counts, warmup):
    """Irreversible growth: the last quarter never falls back to the first
    quarter's maximum. A count that rises and returns is bounded variation."""
    steady = counts[warmup:]
    if len(steady) < 4:
        return {"verdict": "insufficient", "samples": len(steady)}
    quarter = max(1, len(steady) // 4)
    first, last = steady[:quarter], steady[-quarter:]
    if min(steady) == max(steady):
        verdict = "stable"
    elif min(last) > max(first):
        verdict = "growing"
    else:
        verdict = "bounded"
    return {"verdict": verdict, "first": steady[0], "last": steady[-1], "minimum": min(steady),
            "maximum": max(steady), "samples": len(steady)}


def burst_accounting(before_ui, after_ui, before_elf, after_elf, sent, refused):
    """Slider bursts: a value superseded before delivery is rejected as stale by
    design (bridge.c), so the invariant is accepted = delivered + rejected."""
    accepted = after_ui["injected"] - before_ui["injected"]
    delivered = after_elf["ui_events"] - before_elf["ui_events"]
    stale = after_elf["rejected"] - before_elf["rejected"]
    return {"sent": sent, "refused_by_command": refused, "accepted": accepted, "delivered": delivered,
            "stale_rejected": stale, "dropped_at_input": after_ui["input_dropped"] - before_ui["input_dropped"],
            "lost": accepted - delivered - stale}


def find_edge(probe, now, limit_s):
    """Poll an RT1 DIAG probe until the RTC second changes. The edge lies after
    the previous read began and before the next read ended."""
    deadline = now() + limit_s
    previous = probe()
    polls = 1
    while now() < deadline:
        sample = probe()
        polls += 1
        if sample["rtc"] == previous["rtc"]:
            previous = sample
            continue
        if sample["rtc"] != previous["rtc"] + 1 or sample["sync"] != previous["sync"] or \
                sample["at"] != previous["at"]:
            return {"error": "RTC jumped or the clock was set between reads", "polls": polls}
        return {"second": sample["rtc"], "polls": polls, "sync": sample["sync"], "at": sample["at"],
                "mono": [previous["host_mono_before_ns"], sample["host_mono_after_ns"]],
                "wall": [previous["host_wall_before_ns"], sample["host_wall_after_ns"]],
                "raw": [previous.get("host_raw_before_ns"), sample.get("host_raw_after_ns")],
                "device": [previous["mb"] * 1000, sample["ma"] * 1000]}
    return {"error": "no RTC second edge within the poll limit", "polls": polls}


def edge_segments(edges):
    """Split at a device restart or clock set; a fit never spans one."""
    segments = []
    for edge in edges:
        if segments and (edge["sync"], edge["at"]) == (segments[-1][-1]["sync"], segments[-1][-1]["at"]) and \
                edge["device"][0] > segments[-1][-1]["device"][0]:
            segments[-1].append(edge)
        else:
            segments.append([edge])
    return segments


def host_steps(edges):
    """Host CLOCK_REALTIME steps (for example an NTP step) between edges."""
    steps = []
    for first, second in zip(edges, edges[1:]):
        wall = second["wall"][0] - first["wall"][0]
        mono = second["mono"][0] - first["mono"][0]
        if abs(wall - mono) > HOST_STEP_NS:
            steps.append({"second": second["second"], "step_ms": round((wall - mono) / 1e6, 3)})
    return steps


def drift_fit(edges, reference):
    """RTC rate against a reference clock, in ppm (positive: RTC fast).

    bounds_ppm is rigorous: it uses the first and last edge intervals only.
    estimate_ppm is a least-squares fit over all edge midpoints; residual_ms
    shows how well a constant rate explains them."""
    points = [(e["second"], e[reference]) for e in edges if e[reference][0] is not None]
    if len(points) < 2:
        return {"reference": reference, "status": "insufficient", "edges": len(points)}
    (s0, (lo0, hi0)), (s1, (lo1, hi1)) = points[0], points[-1]
    seconds = s1 - s0
    shortest, longest = lo1 - hi0, hi1 - lo0
    if seconds <= 0 or shortest <= 0:
        return {"reference": reference, "status": "insufficient", "edges": len(points)}
    xs = [s - s0 for s, _ in points]
    ys = [((lo + hi) / 2 - (lo0 + hi0) / 2) for _, (lo, hi) in points]
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    spread = sum((x - mean_x) ** 2 for x in xs)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / spread
    intercept = mean_y - slope * mean_x
    residual = math.sqrt(sum((y - intercept - slope * x) ** 2 for x, y in zip(xs, ys)) / len(xs))
    widths = [hi - lo for _, (lo, hi) in points]
    return {"reference": reference, "status": "fitted", "edges": len(points), "span_s": seconds,
            "estimate_ppm": round((1e9 / slope - 1) * 1e6, 4),
            "bounds_ppm": [round((seconds * 1e9 / longest - 1) * 1e6, 4),
                           round((seconds * 1e9 / shortest - 1) * 1e6, 4)],
            "residual_ms": round(residual / 1e6, 3),
            "edge_width_ms": {k: (round(v / 1e6, 3) if isinstance(v, (int, float)) and k != "count" else v)
                              for k, v in percentiles(widths).items()}}


def rtc_analysis(edges):
    good = [e for e in edges if "error" not in e]
    segments = edge_segments(good)
    longest = max(segments, key=len) if segments else []
    steps = host_steps(longest)
    result = {"edges": len(good), "failed_captures": len(edges) - len(good), "segments": len(segments),
              "host_realtime_steps": steps,
              "mono": drift_fit(longest, "mono"), "raw": drift_fit(longest, "raw"),
              "device": drift_fit(longest, "device")}
    result["wall"] = drift_fit(longest, "wall") if not steps else \
        {"reference": "wall", "status": "skipped: host realtime stepped"}
    return result


# The soak level injects no UI input; its delivery check belongs to the stress level.
NOT_APPLICABLE = {"soak": ("delivery",)}


def boot_ids(uptimes):
    """Number boots from 0: a drop in uptime starts a new boot; a missing uptime stays in the current one."""
    ids, boot, last = [], 0, None
    for uptime in uptimes:
        if uptime is not None:
            if last is not None and uptime < last:
                boot += 1
            last = uptime
        ids.append(boot)
    return ids


def evaluate(analysis, monitoring, phases):
    """Apply CRITERIA. A criterion without data is UNTESTED, never PASS."""
    results = {}
    problems = [p for phase in phases for p in phase["details"].get("problems", [])]
    results["resets"] = FAIL if monitoring["unexpected_resets"] else PASS
    results["faults"] = FAIL if monitoring["faults"] else PASS
    results["integrity"] = FAIL if any(not p.startswith("delivery:") for p in problems) else PASS
    delivery = analysis.get("delivery")
    if not delivery or not delivery["checked"]:
        results["delivery"] = "UNTESTED"
    else:
        results["delivery"] = FAIL if delivery["lost"] or delivery["undelivered_spaced"] else PASS
    allocation = analysis.get("alloc_failed_growth")
    results["allocation"] = "UNTESTED" if allocation is None else (FAIL if allocation else PASS)
    tasks = [t["verdict"] for t in analysis.get("tasks", {}).values()]
    results["tasks"] = "UNTESTED" if not tasks or all(v == "insufficient" for v in tasks) else \
        (FAIL if "growing" in tasks else PASS)
    verdicts = [r["verdict"] for series in analysis.get("memory", {}).values() for r in series.values()
                if isinstance(r, dict) and "verdict" in r]
    results["memory"] = "UNTESTED" if not verdicts or all(v == "insufficient" for v in verdicts) else \
        (FAIL if "leak" in verdicts else PASS)
    results["communication"] = FAIL if monitoring.get("lost") else PASS
    return results


# ---- runner -----------------------------------------------------------------

class EnduranceRunner(m8.Runner):
    def __init__(self, args, device, monitor, out, sleep=time.sleep, now=time.monotonic):
        super().__init__(args, device, monitor, out, sleep, now)
        self.t0 = now()
        self.deadline = self.t0 + args.max_hours * 3600
        self.staged = None
        self.stop_reason = None
        self.samples = []
        self.budget_supported = True
        self.edges = []
        self.latency = {"start_ms": {}, "close_ms": {}, "event_ms": [], "ui_input_ms": []}
        self.delivery = {"checked": False, "spaced_sent": 0, "spaced_delivered": 0, "undelivered_spaced": 0,
                         "invalid_sent": 0, "invalid_delivered": 0, "lost": 0, "bursts": []}
        self.counts = {"lifecycle_cycles": 0, "app_runs": 0, "ui_rounds": 0, "injected_inputs": 0,
                       "events_sent": 0, "rtc_captures": 0}
        self.report.update(runner=f"endurance {RUNNER_VERSION} (run_m8 {m8.RUNNER_VERSION})",
                           level=args.level, criteria=[{"name": n, "rule": r} for n, r in CRITERIA],
                           host_time_sync={"start": m8.clock.host_sync_state()}, counts=self.counts)
        self.report["monitoring"]["tasks"] = "RS1 HEALTH (see analysis.tasks)"
        self.report["monitoring"]["lost"] = []
        self.report["manual"] = []

    # ---- reporting ---------------------------------------------------------
    def write_report(self):
        self.report["monitoring"]["faults"] = list(self.monitor.faults)
        self.report["monitoring"]["notices"] = list(self.monitor.notices)
        self.out.mkdir(parents=True, exist_ok=True)
        for name, text in (("report.json", json.dumps(self.report, indent=2) + "\n"),
                           ("report.md", markdown(self.report))):
            temporary = self.out / (name + ".tmp")
            temporary.write_text(text)
            temporary.replace(self.out / name)

    def elapsed_s(self):
        return round(self.now() - self.t0, 3)

    def log_bytes(self):
        return sum((self.out / n).stat().st_size for n in ("serial.log", "telemetry.jsonl")
                   if (self.out / n).exists())

    def limited(self):
        """True once a run limit is reached; loops end at the next safe point."""
        if self.stop_reason is None:
            if self.now() >= self.deadline:
                self.stop_reason = f"--max-hours {self.args.max_hours} reached"
            elif self.log_bytes() > self.args.max_log_mb * 1024 * 1024:
                self.stop_reason = f"--max-log-mb {self.args.max_log_mb} reached"
        return self.stop_reason is not None

    # ---- telemetry ---------------------------------------------------------
    def observe_uptime(self, label):
        """As in run_m8, but a transport error propagates (the caller reconnects)
        instead of switching reset detection off for the rest of a long run."""
        monitoring = self.report["monitoring"]
        try:
            sample = self.call("clock", None)
        except DeviceError as error:
            monitoring["uptime"].append({"label": label, "error": str(error)})
            return None
        uptime = sample["uptime_us"]
        reset = self.last_uptime_us is not None and uptime < self.last_uptime_us
        if reset:
            if self.expected_resets > 0:
                self.expected_resets -= 1
            else:
                monitoring["unexpected_resets"].append(
                    {"label": label, "before_us": self.last_uptime_us, "after_us": uptime})
        self.last_uptime_us = uptime
        if reset or len(monitoring["uptime"]) < 50:
            monitoring["uptime"].append({"label": label, "uptime_us": uptime, "reset": reset})
        return uptime

    def snapshot(self, label, state):
        """One telemetry record. state names the device state, so series compare
        like with like (list: launcher list, no application; home: watch)."""
        record = {"t_s": self.elapsed_s(), "utc": utc_now(), "label": label, "state": state}
        health = self.call("safe", "HEALTH")
        if all(key in health for key in HEALTH_KEYS):
            record["health"] = {key: health[key] for key in HEALTH_KEYS}
        else:
            record["health"] = None
        elf_status = self.call("elf")
        record["elf"] = {key: elf_status.get(key) for key in ELF_KEYS}
        ui_status = self.call("ui")
        record["ui"] = {key: ui_status.get(key) for key in UI_KEYS}
        record["budget"] = self.budget_sample()
        record["uptime_us"] = self.observe_uptime(label)
        self.samples.append(record)
        with open(self.out / "telemetry.jsonl", "a") as stream:
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")
        return record

    def budget_sample(self):
        """Boot-wide callback budget counters (RE1 BUDGET, M9); None on older
        firmware, which is then not asked again."""
        if not self.budget_supported:
            return None
        try:
            return self.call("budget")["boot"]
        except m8.budget.Unsupported:
            self.budget_supported = False
            return None

    # ---- launcher-driven applications -------------------------------------
    def to_list(self):
        """Reach a launcher list that stays put with an idle runtime. Ending a
        non-home application relaunches home (launcher.c), so close that too."""
        for _ in range(8):
            nav = self.settled_nav()
            if nav["state"] == "RESULT":
                self.call("launcher", f"BACK {nav['request']}")
                continue
            if nav["state"] == "ACTIVE":
                self.call("launcher", f"EXIT {nav['generation']}")
                self.wait_for("launcher list", lambda: self.call("launcher", "NAV"),
                              lambda n: n["state"] in ("LIST", "RESULT", "ERROR"), 8)
                continue
            if nav["state"] != "LIST":
                raise DeviceError(f"launcher is {nav['state']}")
            settle_until = self.now() + LIST_SETTLE_S
            settled = True
            while self.now() < settle_until:
                nav = self.call("launcher", "NAV")
                runtime = self.call("elf")
                if nav["state"] != "LIST" or runtime["busy"] or runtime["state"] != ELF_IDLE:
                    settled = False
                    break
                self.sleep(0.25)
            if settled:
                return nav
        raise DeviceError("launcher did not settle on its list")

    def ensure_event_staged(self):
        info = self.inspect("event")
        if self.staged != info["sha256"]:
            self.stage("event")
            self.staged = info["sha256"]
        return info["sha256"]

    def launch(self, name):
        """Start one application from a settled list and close it again.
        Returns (sample, problems)."""
        nav = self.to_list()
        before = self.call("elf")
        problems = []
        sample = {"app": name}
        if name == "event":
            digest = self.ensure_event_staged()  # outside the timing: a Flash write when first staged
        started = self.now()
        if name == "event":
            status = self.device.session(lambda port: m8.elf.start(port, digest))
            self.loaded = status["instance"]
            sample["start_ms"] = round((self.now() - started) * 1000, 1)
            users = self.call("elf")["users"]
            sent = self.now()
            self.call("elf", f"EVENT {self.loaded} 4321")
            self.counts["events_sent"] += 1
            after_event = self.wait_for("user event", lambda: self.call("elf"),
                                        lambda s: s["users"] >= users + 1, 4, 0.02)
            self.latency["event_ms"].append(round((self.now() - sent) * 1000, 1))
            if after_event["value"] != 4321:
                problems.append(f"event: value {after_event['value']} delivered, 4321 sent")
            instance = self.loaded
            closing = self.now()
            unloaded = self.stop()
            sample["close_ms"] = round((self.now() - closing) * 1000, 1)
        else:
            self.call("launcher", f"SELECT {nav['revision']} {PACKAGES[name]}")
            if name == "hello":
                result = self.wait_for("hello result", lambda: self.call("launcher", "NAV"),
                                       lambda n: n["state"] in ("RESULT", "ERROR"), 10, 0.05)
                sample["start_ms"] = round((self.now() - started) * 1000, 1)
                runtime = self.call("elf")
                instance = runtime["instance"]
                if runtime["app_result"] != 42 or not runtime["executed"]:
                    problems.append(f"hello: result {runtime['app_result']} executed {runtime['executed']}")
                closing = self.now()
                self.call("launcher", f"BACK {result['request']}")
            else:
                active = self.wait_for(f"{name} active", lambda: self.call("launcher", "NAV"),
                                       lambda n: n["state"] in ("ACTIVE", "RESULT", "ERROR") and
                                       n["selected"] == PACKAGES[name], 10, 0.05)
                if active["state"] != "ACTIVE":
                    raise DeviceError(f"{name} did not start: launcher {active['state']}")
                instance = self.call("elf")["instance"]
                self.wait_for(f"{name} widgets", lambda: self.call("ui"),
                              lambda s: s["generation"] == instance and s["widgets"] > 0 and s["pending"] == 0,
                              6, 0.05)
                sample["start_ms"] = round((self.now() - started) * 1000, 1)
                closing = self.now()
                self.call("launcher", f"EXIT {active['generation']}")
            # Ending a non-home application relaunches home at once, so the
            # runtime may be busy again; completion of our instance is the signal.
            unloaded = self.wait_for(f"{name} closed", lambda: self.call("elf"),
                                     lambda s: s["completed"] >= instance and
                                     (s["instance"] != instance or s["state"] == ELF_IDLE), 10, 0.05)
            sample["close_ms"] = round((self.now() - closing) * 1000, 1)
        ui_after = self.call("ui")
        ours = unloaded["instance"] == instance
        if ui_after["generation"] in (0, unloaded["instance"]) and ours and \
                (ui_after["widgets"] != 0 or ui_after["outstanding"] != 0):
            problems.append(f"{name}: widgets or UI resources left after unload")
        if ours:
            # The status still describes our instance: check what it left.
            if unloaded["resources"][3] != 0:
                problems.append(f"{name}: {unloaded['resources'][3]} SDK resources outstanding")
            if unloaded["error"]:
                problems.append(f"{name}: application error {unloaded['error']!r}")
            sample.update(load_us=unloaded["load_us"], unload_us=unloaded["unload_us"], after=unloaded["after"])
        else:
            # Home already replaced it; the launcher relaunches home only after
            # a clean unload (launcher.c), which is the integrity evidence here.
            sample["superseded_by_home"] = True
        for key in ("rejected", "dropped", "over_budget"):
            if unloaded[key] != before[key]:
                problems.append(f"{name}: executor {key} {before[key]} -> {unloaded[key]}")
        sample.update(instance=instance, lvgl_bytes=ui_after["lvgl_bytes"])
        self.latency["start_ms"].setdefault(name, []).append(sample["start_ms"])
        self.latency["close_ms"].setdefault(name, []).append(sample["close_ms"])
        self.counts["app_runs"] += 1
        return sample, problems

    def lifecycle(self, cycles, label):
        samples, problems = [], []
        for cycle in range(cycles):
            if self.limited():
                break
            for name in LIFECYCLE:
                sample, found = self.launch(name)
                sample["cycle"] = cycle
                samples.append(sample)
                problems += [f"cycle {cycle}: {p}" for p in found]
            self.to_list()
            self.snapshot(f"{label} cycle {cycle}", "list")
            self.counts["lifecycle_cycles"] += 1
            self.write_report()
        return samples, problems

    # ---- scenarios ---------------------------------------------------------
    def s_baseline(self):
        record = self.snapshot("baseline", "as found")
        if record["health"] is None:
            return BLOCKED, {"snapshot": record,
                             "reason": "RS1 HEALTH not available; task, minimum-heap and allocation "
                                       "checks are UNTESTED"}
        return PASS, {"snapshot": record}

    def s_lifecycle(self):
        samples, problems = self.lifecycle(self.args.cycles, "lifecycle")
        details = {"cycles_requested": self.args.cycles, "cycles_run": len({s["cycle"] for s in samples}),
                   "apps": list(LIFECYCLE), "samples": samples, "problems": problems,
                   "origin": "launcher SELECT/EXIT/BACK (INJECTED) and RE1 for the event example"}
        if self.stop_reason:
            details["stopped"] = self.stop_reason
        return (FAIL if problems else PASS), details

    def ui_input(self, g, operation, accept, what):
        before = (self.call("ui"), self.call("elf"))
        sent = self.now()
        self.call("ui", operation)
        self.counts["injected_inputs"] += 1
        self.delivery["spaced_sent"] += 1
        result = self.wait_for(what, lambda: (self.call("ui"), self.call("elf")),
                               lambda pair: pair[1]["ui_events"] >= before[1]["ui_events"] + 1 and
                               accept(pair[0], pair[1]), 4, 0.02)
        self.latency["ui_input_ms"].append(round((self.now() - sent) * 1000, 1))
        self.delivery["spaced_delivered"] += result[1]["ui_events"] - before[1]["ui_events"]
        return result

    def invalid_input(self, operation):
        """Must be refused and never reach the application."""
        before = (self.call("ui"), self.call("elf"))
        refused = False
        try:
            self.call("ui", operation)
        except DeviceError:
            refused = True
        self.sleep(0.3)
        after = (self.call("ui"), self.call("elf"))
        self.delivery["invalid_sent"] += 1
        delivered = after[1]["ui_events"] - before[1]["ui_events"]
        self.delivery["invalid_delivered"] += delivered
        return {"operation": operation, "refused_by_command": refused,
                "input_rejected": after[0]["input_rejected"] - before[0]["input_rejected"], "delivered": delivered}

    def s_ui_stress(self):
        nav = self.to_list()
        self.call("launcher", f"SELECT {nav['revision']} {PACKAGES['round']}")
        self.wait_for("round active", lambda: self.call("launcher", "NAV"),
                      lambda n: n["state"] == "ACTIVE" and n["selected"] == PACKAGES["round"], 10, 0.05)
        g = self.call("elf")["instance"]
        created, h = self.round_handles(g)
        self.delivery["checked"] = True
        problems, rounds = [], []
        level_text = lambda value: (lambda u, e: u["text_hash"] == fnv1a(f"Level: {value}"))  # noqa: E731
        no_error = lambda u, e: e["error"] == ""  # noqa: E731
        elf_start = self.call("elf")
        for index in range(self.args.ui_rounds):
            if self.limited():
                break
            rejected_start = self.call("elf")["rejected"]
            sent_before = self.delivery["spaced_sent"]
            delivered_before = self.delivery["spaced_delivered"]
            for item in (3, 0, index % len(COLORS)):
                self.ui_input(g, f"VALUE {g} {h['list']} {item}", no_error, f"list {COLORS[item]}")
            for level in (LEVEL_MIN, LEVEL_MAX, 7 + (index * 13) % 90):
                self.ui_input(g, f"VALUE {g} {h['slider']} {level}", level_text(level), f"slider {level}")
            self.ui_input(g, f"PRESS {g} {h['edge']}", lambda u, e: u["pending"] == 0, "dialog shown")
            self.ui_input(g, f"VALUE {g} {h['dialog']} 1", level_text(40), "dialog confirm")
            self.ui_input(g, f"VALUE {g} {h['slider']} 55", level_text(55), "slider 55")
            self.ui_input(g, f"PRESS {g} {h['edge']}", lambda u, e: u["pending"] == 0, "dialog shown")
            self.ui_input(g, f"VALUE {g} {h['dialog']} 2", level_text(55), "dialog cancel")
            invalid = [self.invalid_input(op) for op in (f"VALUE {g} {h['slider']} {LEVEL_MAX + 1}",
                                                         f"VALUE {g} {h['slider']} {LEVEL_MIN - 1}",
                                                         f"VALUE {g} {h['list']} {len(COLORS)}",
                                                         f"VALUE {g} {h['dialog']} 3")]
            for entry in invalid:
                if entry["delivered"]:
                    problems.append(f"round {index}: invalid input {entry['operation']!r} was delivered")
            # Burst: values back to back, no wait in between.
            ui0, elf0 = self.call("ui"), self.call("elf")
            values = [11 + (index + k * 17) % 80 for k in range(self.args.burst)]
            refused = 0
            for value in values:
                try:
                    self.call("ui", f"VALUE {g} {h['slider']} {value}")
                except DeviceError:
                    refused += 1
            self.counts["injected_inputs"] += len(values)
            ui1, elf1 = self.wait_for("burst settled", lambda: (self.call("ui"), self.call("elf")),
                                      lambda pair: pair[1]["ui_events"] + pair[1]["rejected"] -
                                      elf0["ui_events"] - elf0["rejected"] >= pair[0]["injected"] - ui0["injected"]
                                      and pair[0]["text_hash"] == fnv1a(f"Level: {values[-1]}"), 6, 0.05)
            burst = burst_accounting(ui0, ui1, elf0, elf1, len(values), refused)
            self.delivery["bursts"].append(burst)
            self.delivery["lost"] += burst["lost"]
            if burst["lost"]:
                problems.append(f"delivery: round {index}: {burst['lost']} accepted burst input(s) lost")
            spaced_sent = self.delivery["spaced_sent"] - sent_before
            spaced_delivered = self.delivery["spaced_delivered"] - delivered_before
            if spaced_delivered != spaced_sent:
                self.delivery["undelivered_spaced"] += spaced_sent - spaced_delivered
                problems.append(f"delivery: round {index}: {spaced_sent} spaced inputs, {spaced_delivered} delivered")
            if elf0["rejected"] != rejected_start:
                problems.append(f"round {index}: executor rejected {elf0['rejected'] - rejected_start} "
                                "spaced or invalid input(s)")
            if elf1["error"]:
                problems.append(f"round {index}: application error {elf1['error']!r}")
            rounds.append({"round": index, "spaced_sent": spaced_sent, "spaced_delivered": spaced_delivered,
                           "invalid": invalid, "burst": burst,
                           "input_latency_max_us": ui1["input_latency_max_us"]})
            self.counts["ui_rounds"] += 1
            self.write_report()
        elf_end = self.call("elf")
        nav = self.call("launcher", "NAV")
        self.call("launcher", f"EXIT {nav['generation']}")
        self.wait_for("round closed", lambda: self.call("elf"),
                      lambda s: s["completed"] >= g and (s["instance"] != g or s["state"] == ELF_IDLE), 10, 0.05)
        ui_after = self.call("ui")
        if ui_after["generation"] in (0, g) and (ui_after["widgets"] or ui_after["outstanding"]):
            problems.append("round: widgets left after unload")
        if elf_end["error"]:
            problems.append(f"round: application error {elf_end['error']!r}")
        if elf_end["dropped"] != elf_start["dropped"] or elf_end["over_budget"] != elf_start["over_budget"]:
            problems.append("round: executor dropped events or a callback ran over budget")
        self.to_list()
        self.snapshot("after ui stress", "list")
        details = {"generation": g, "handles": h, "created": created["widgets"], "rounds": rounds,
                   "problems": problems, "origin": "INJECTED through RE1 UI; not CST816S touch",
                   "lvgl_bytes_after": ui_after["lvgl_bytes"]}
        return (FAIL if problems else PASS), details

    def s_recovery(self):
        details, problems = {}, []
        self.to_list()
        status, bad = self.s_bad_package()
        self.staged = None  # bad_package staged other files
        details["bad_package"] = {"status": status, "details": bad}
        if status != PASS:
            problems.append("bad package was not refused cleanly or the good application did not run after it")
        # Invalid lifecycle input: a stale instance must be refused.
        self.to_list()
        digest = self.ensure_event_staged()
        started = self.device.session(lambda port: m8.elf.start(port, digest))
        self.loaded = started["instance"]
        users = self.call("elf")["users"]
        stale = {}
        for operation in (f"EVENT {self.loaded + 1000} 7", f"STOP {self.loaded + 1000}"):
            try:
                self.call("elf", operation)
                stale[operation.split()[0]] = "accepted"
            except DeviceError:
                stale[operation.split()[0]] = "refused"
        if self.call("elf")["users"] != users or "accepted" in stale.values():
            problems.append(f"stale-instance commands were not refused: {stale}")
        details["stale_instance"] = stale
        # App restart: the same application, five times back to back.
        restarts = []
        for _ in range(5):
            self.stop()
            # The launcher relaunches home after any application ends, also
            # one started through RE1, so return to a settled list first.
            self.to_list()
            again = self.device.session(lambda port: m8.elf.start(port, digest))
            self.loaded = again["instance"]
            restarts.append(again["instance"])
        unloaded = self.stop()
        if unloaded["error"] or unloaded["resources"][3]:
            problems.append("restarted application left an error or resources")
        details["app_restarts"] = restarts
        # A stale UI generation after the application ended.
        try:
            self.call("ui", f"PRESS {restarts[0]} 1")
            details["stale_ui"] = "accepted"
            problems.append("an input for an ended generation was accepted")
        except DeviceError:
            details["stale_ui"] = "refused"
        # Serial reopen: twenty quick open/close cycles of the port.
        answered = 0
        for _ in range(20):
            self.call("ota")
            answered += 1
        details["serial_reopen"] = answered
        if self.args.restart:
            status, restart = self.s_restart()
            details["restart"] = {"status": status, "details": restart}
            if status != PASS:
                problems.append("software restart failed")
            details["after_restart"] = self.snapshot("after restart", "home")
        else:
            details["restart"] = "not requested (--restart)"
        details["problems"] = problems
        return (FAIL if problems else PASS), details

    def rtc_edge(self, predicted_ns=None):
        """One RTC second edge against host clocks; the RTC is only read."""
        if predicted_ns is not None:
            wait = (predicted_ns - time.monotonic_ns()) / 1e9 - EDGE_LEAD_S
            if wait > 0:
                time.sleep(min(wait, 1.0))

        def burst(port):
            def probe():
                raw_before = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
                sample = m8.clock.query(port, 3.0)
                sample.update(host_raw_before_ns=raw_before,
                              host_raw_after_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW))
                return sample
            return find_edge(probe, time.monotonic, EDGE_LIMIT_S)

        edge = self.device.session(burst)
        edge["t_s"] = self.elapsed_s()
        self.edges.append(edge)
        self.counts["rtc_captures"] += 1
        with open(self.out / "telemetry.jsonl", "a") as stream:
            stream.write(json.dumps({"rtc_edge": edge}, separators=(",", ":")) + "\n")
        return edge

    def next_edge_prediction(self):
        good = [e for e in self.edges if "error" not in e]
        if not good:
            return None
        last = good[-1]
        middle = (last["mono"][0] + last["mono"][1]) // 2
        now_ns = time.monotonic_ns()
        return middle + max(1, math.ceil((now_ns - middle) / 1e9)) * 1_000_000_000

    def s_soak(self):
        """Idle with the home application; periodic telemetry, RTC edges and
        lifecycle batches."""
        self.return_home()
        end = min(self.deadline, self.now() + self.args.hours * 3600)
        next_sample = self.now()
        next_edge = self.now() if self.args.rtc_every else None
        next_batch = self.now() + self.args.batch_every if self.args.batch_every else None
        problems, batches = [], []
        idle_samples = 0
        while self.now() < end and not self.limited():
            if self.now() >= next_sample:
                next_sample += self.args.interval
                try:
                    record = self.snapshot("soak", "home")
                except (TransportError, OSError) as error:
                    # A USB loss is recorded and survived if the board answers
                    # again; otherwise the step ends as a communication loss.
                    lost_at = self.elapsed_s()
                    self.reconnect(self.args.reconnect_timeout)
                    self.report["monitoring"].setdefault("reconnects", []).append(
                        {"step": "soak", "at_s": lost_at, "back_s": self.elapsed_s(), "error": str(error)})
                    continue
                idle_samples += 1
                elf_status = record["elf"]
                if elf_status["error"]:
                    problems.append(f"home application error {elf_status['error']!r} at {record['t_s']} s")
            if next_edge is not None and self.now() >= next_edge:
                edge = self.rtc_edge(self.next_edge_prediction())
                if "error" in edge:
                    problems.append(f"RTC edge capture: {edge['error']}")
                next_edge += self.args.rtc_every
            if next_batch is not None and self.now() >= next_batch:
                samples, found = self.lifecycle(self.args.batch_cycles, "soak batch")
                batches.append({"t_s": self.elapsed_s(), "cycles": len({s["cycle"] for s in samples}),
                                "problems": found})
                problems += found
                self.return_home()
                next_batch += self.args.batch_every
            self.write_report()
            pending = [t for t in (next_sample, next_edge, next_batch, end) if t is not None]
            self.sleep(max(0.0, min(pending) - self.now()))
        details = {"hours_requested": self.args.hours, "hours_run": round((self.now() - self.t0) / 3600, 3),
                   "samples": idle_samples, "rtc_edges": len(self.edges), "batches": batches,
                   "problems": [p for p in problems if not p.startswith("RTC edge")],
                   "rtc_problems": [p for p in problems if p.startswith("RTC edge")]}
        if self.stop_reason:
            details["stopped"] = self.stop_reason
        return (FAIL if details["problems"] else PASS), details

    LEVELS = {"smoke": ("baseline", "lifecycle", "ui_stress", "recovery", "soak"),
              "stress": ("baseline", "lifecycle", "ui_stress", "recovery"),
              "soak": ("baseline", "soak")}

    def run_scenario(self, name):
        if self.disconnected:
            return self.record(name, BLOCKED, {"reason": "device unavailable after an earlier loss"})
        entry = super().run_scenario(name)
        details = entry["details"]
        if details.get("reconnected") is False:
            self.report["monitoring"]["lost"].append({"step": name, "error": details.get("error")})
        elif details.get("reconnected") is True:
            self.report["monitoring"].setdefault("reconnects", []).append({"step": name,
                                                                          "error": details.get("error")})
        self.write_report()
        return entry

    # ---- analysis ----------------------------------------------------------
    def analyze(self):
        analysis = {"counts": self.counts}
        latency = {"start_ms": {k: percentiles(v) for k, v in self.latency["start_ms"].items()},
                   "close_ms": {k: percentiles(v) for k, v in self.latency["close_ms"].items()},
                   "event_ms": percentiles(self.latency["event_ms"]),
                   "ui_input_ms": percentiles(self.latency["ui_input_ms"])}
        lifecycle = [s for step in self.report["steps"] if step["name"] == "lifecycle"
                     for s in step["details"].get("samples", [])]
        latency["device_load_us"] = percentiles([s["load_us"] for s in lifecycle if s.get("load_us")])
        latency["device_unload_us"] = percentiles([s["unload_us"] for s in lifecycle if s.get("unload_us")])
        latency["device_input_max_us"] = max([s["ui"]["input_latency_max_us"] or 0 for s in self.samples],
                                             default=None)
        analysis["latency"] = latency
        analysis["delivery"] = dict(self.delivery, bursts=len(self.delivery["bursts"]),
                                    burst_totals={k: sum(b[k] for b in self.delivery["bursts"])
                                                  for k in ("sent", "accepted", "delivered", "stale_rejected",
                                                            "dropped_at_input", "lost")})
        with_health = [s for s in self.samples if s["health"] is not None]
        if with_health:
            failed = [s["health"]["alloc_failed"] for s in with_health]
            analysis["alloc_failed_growth"] = failed[-1] - failed[0]
        else:
            analysis["alloc_failed_growth"] = None
        analysis["memory"], analysis["tasks"], analysis["stacks_min"] = {}, {}, {}
        boots = boot_ids([s["uptime_us"] for s in self.samples])
        boot_of = {id(s): boot for s, boot in zip(self.samples, boots)}
        for boot in sorted(set(boots)):
            for state in ("list", "home"):
                series = [s for s in with_health if s["state"] == state and boot_of[id(s)] == boot]
                if series:
                    key = state if len(set(boots)) == 1 else f"{state} (boot {boot})"
                    self.analyze_state(analysis, key, series)
        for s in with_health:
            for task, mark in s["health"]["stacks"].items():
                if mark >= 0:
                    analysis["stacks_min"][task] = min(mark, analysis["stacks_min"].get(task, mark))
        analysis["rtc"] = rtc_analysis(self.edges)
        analysis["host_time_sync"] = self.report["host_time_sync"]
        return analysis

    def analyze_state(self, analysis, key, series):
        """Leak and task verdicts for one device state within one boot."""
        warmup = min(self.args.warmup, max(0, len(series) - 5))
        times = [s["t_s"] for s in series]
        memory = {}
        for metric in ("internal_free", "internal_largest", "psram_free"):
            values = [s["health"][metric] for s in series]
            memory[metric] = dict(leak_check(values, warmup, LEAK_TOLERANCE[metric]),
                               slope_per_hour=slope_per_hour(times, values))
        lvgl = [s["ui"]["lvgl_bytes"] for s in series]
        memory["lvgl_bytes"] = dict(leak_check([-v for v in lvgl], warmup, LEAK_TOLERANCE["lvgl_bytes"]),
                                    note="growth of LVGL bytes, checked as a decline of its negative")
        memory["internal_min_last"] = series[-1]["health"]["internal_min"]
        memory["psram_min_last"] = series[-1]["health"]["psram_min"]
        analysis["memory"][key] = memory
        analysis["tasks"][key] = dict(task_stability([s["health"]["tasks"] for s in series], warmup),
                                      values=sorted({s["health"]["tasks"] for s in series}))


def markdown(report):
    analysis = report.get("analysis") or {}
    device = report["device"]
    lines = [f"# M8.1 endurance — {report.get('level')} — {device.get('board_detected', '?')}", "",
             f"- Started {report['started']}, finished {report['finished'] or 'not finished'}",
             f"- Firmware {device.get('firmware')} in {device.get('running')}",
             f"- Preflight: {report['preflight'].get('status', 'not run')}",
             f"- Outcome: {report['outcome'] or 'in progress'}"]
    if report.get("stopped"):
        lines.append(f"- Stopped early: {report['stopped']}")
    lines += ["", "| Phase | Status | Duration (s) | Notes |", "|---|---|---|---|"]
    for step in report["steps"]:
        details = step["details"]
        note = details.get("reason") or details.get("error") or "; ".join(details.get("problems", [])[:3])
        lines.append(f"| {step['name']} | {step['status']} | {details.get('duration_s', '')} | {str(note)[:200]} |")
    if report.get("criteria_results"):
        lines += ["", "| Criterion | Result | Rule |", "|---|---|---|"]
        rules = {c["name"]: c["rule"] for c in report["criteria"]}
        for name, result in report["criteria_results"].items():
            lines.append(f"| {name} | {result} | {rules[name]} |")
    if analysis:
        lines += ["", "## Counts", "", "```", json.dumps(analysis.get("counts"), indent=1), "```",
                  "", "## Latency", "", "```", json.dumps(analysis.get("latency"), indent=1), "```",
                  "", "## Memory and tasks", "", "```",
                  json.dumps({"memory": analysis.get("memory"), "tasks": analysis.get("tasks"),
                              "stacks_min": analysis.get("stacks_min"),
                              "alloc_failed_growth": analysis.get("alloc_failed_growth")}, indent=1), "```",
                  "", "## Delivery", "", "```", json.dumps(analysis.get("delivery"), indent=1), "```",
                  "", "## RTC", "", "```", json.dumps(analysis.get("rtc"), indent=1), "```"]
    monitoring = report["monitoring"]
    lines += ["", f"- Unexpected resets: {len(monitoring['unexpected_resets'])}",
              f"- Serial faults: {len(monitoring['faults'])}",
              f"- Serial notices: {len(monitoring['notices'])}",
              f"- Communication losses without reconnect: {len(monitoring.get('lost', []))}",
              f"- Restore: {'; '.join(report['restore']) or 'nothing to restore'}", ""]
    for fault in monitoring["faults"][:20]:
        lines.append(f"  - fault at {fault['at_s']} s: `{fault['line']}`")
    return "\n".join(lines) + "\n"


def outcome(report):
    if report["preflight"].get("status") != PASS:
        return BLOCKED
    statuses = [step["status"] for step in report["steps"]]
    results = report.get("criteria_results", {}).values()
    if FAIL in statuses or FAIL in results or report.get("restore_failed"):
        return FAIL
    if BLOCKED in statuses or report.get("stopped") or "UNTESTED" in results:
        return "INCOMPLETE"
    return PASS


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--level", choices=("smoke", "stress", "soak"), required=True)
    parser.add_argument("--port")
    parser.add_argument("--serial")
    parser.add_argument("--board", choices=m8.BOARDS)
    parser.add_argument("--out")
    parser.add_argument("--apps", default=str(m8.ROOT / "build-ci/m8-apps"))
    parser.add_argument("--cycles", type=int, default=None, help="lifecycle cycles of all six apps")
    parser.add_argument("--ui-rounds", type=int, default=None)
    parser.add_argument("--burst", type=int, default=8, help="slider values per burst")
    parser.add_argument("--hours", type=float, default=None, help="soak duration")
    parser.add_argument("--interval", type=float, default=None, help="soak telemetry interval (s)")
    parser.add_argument("--rtc-every", type=float, default=None, help="RTC edge capture interval (s); 0: off")
    parser.add_argument("--batch-every", type=float, default=3600.0, help="soak lifecycle batch interval (s); 0: off")
    parser.add_argument("--batch-cycles", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--restart", action="store_true", help="include one RS1/RO1 software restart in recovery")
    parser.add_argument("--max-hours", type=float, default=14.0, help="hard limit for the whole run")
    parser.add_argument("--max-log-mb", type=float, default=200.0)
    parser.add_argument("--reconnect-timeout", type=float, default=30.0)
    args = parser.parse_args(argv)
    # cycles, ui rounds, soak hours, telemetry interval, RTC edge interval
    defaults = {"smoke": (2, 2, 0.05, 20.0, 30.0), "stress": (100, 30, 0.0, 60.0, 0.0),
                "soak": (0, 0, 10.0, 60.0, 300.0)}[args.level]
    for index, key in enumerate(("cycles", "ui_rounds", "hours", "interval", "rtc_every")):
        if getattr(args, key) is None:
            setattr(args, key, defaults[index])
    if not 0 <= args.cycles <= 1000 or not 0 <= args.ui_rounds <= 500 or not 1 <= args.burst <= 32:
        parser.error("--cycles 0..1000, --ui-rounds 0..500, --burst 1..32")
    if not 0 <= args.hours <= 24 or not 0 < args.max_hours <= 26 or args.interval < 5:
        parser.error("--hours 0..24, --max-hours 0..26, --interval at least 5 s")
    if (args.rtc_every and args.rtc_every < 10) or (args.batch_every and args.batch_every < 60):
        parser.error("--rtc-every at least 10 s, --batch-every at least 60 s")
    # Attributes the reused run_m8 steps read.
    args.light_sleep = False
    args.rtc_sync = False
    args.dialog_rounds = 1
    args.sleep_seconds = 5
    args.ota = None
    return args


def main(argv=None, device_factory=m8.Device, by_id=m8.BY_ID, sleep=time.sleep, now=time.monotonic):
    args = parse_args(argv)
    out = Path(args.out or m8.ROOT / "build-ci/endurance" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    out.mkdir(parents=True, exist_ok=True)
    monitor = m8.Monitor(out / "serial.log", now)
    runner = None

    def terminate(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, terminate)
    try:
        try:
            path, serial = m8.discover(args.port, args.serial, by_id)
        except Blocked as error:
            blocked = EnduranceRunner(args, None, monitor, out, sleep, now)
            blocked.report["preflight"] = {"status": BLOCKED, "problems": [str(error)]}
            blocked.report["outcome"] = BLOCKED
            blocked.report["finished"] = utc_now()
            blocked.write_report()
            print(f"BLOCKED: {error}", file=sys.stderr)
            return 2
        runner = EnduranceRunner(args, device_factory(path, monitor), monitor, out, sleep, now)
        try:
            ready, _ = runner.preflight(serial)
        except (TransportError, DeviceError, OSError) as error:
            runner.report["preflight"] = {"status": BLOCKED, "problems": [f"no valid Roundie reply: {error}"]}
            ready = False
        if not ready:
            runner.report["outcome"] = BLOCKED
            return 2
        for name in EnduranceRunner.LEVELS[args.level]:
            runner.run_scenario(name)
        runner.restore()
        return {PASS: 0, BLOCKED: 2}.get(finish(runner), 1)
    except KeyboardInterrupt:
        if runner is not None:
            runner.report["interrupted"] = True
            runner.stop_reason = runner.stop_reason or "interrupted (SIGTERM or Ctrl-C)"
            runner.restore()
            finish(runner)
        return 130
    finally:
        signal.signal(signal.SIGTERM, previous)
        if runner is not None:
            runner.report["finished"] = utc_now()
            runner.write_report()
            print(markdown(runner.report))
        monitor.close()


def finish(runner):
    report = runner.report
    report["stopped"] = runner.stop_reason
    report["host_time_sync"]["end"] = m8.clock.host_sync_state()
    report["analysis"] = runner.analyze()
    report["criteria_results"] = evaluate(report["analysis"], report["monitoring"], report["steps"])
    for name in NOT_APPLICABLE.get(report["level"], ()):
        if report["criteria_results"][name] == "UNTESTED":
            report["criteria_results"][name] = "NOT APPLICABLE"
    report["outcome"] = outcome(report)
    return report["outcome"]


if __name__ == "__main__":
    sys.exit(main())
