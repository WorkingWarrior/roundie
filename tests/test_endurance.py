import contextlib
import functools
import io
import json
from pathlib import Path
import sys
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools" / "hardware_validation"))
import endurance  # noqa: E402
from endurance import FAIL, PASS  # noqa: E402
from test_run_m8 import SERIAL, FakeDevice, RunnerTestCase  # noqa: E402


class HealthDevice(FakeDevice):
    """FakeDevice with RS1 HEALTH; tests change internal_free and tasks per call."""

    def __init__(self, clock, hook=None):
        super().__init__(clock, hook=hook)
        self.health = dict(uptime_us=0, internal_free=150000, internal_min=120000, internal_largest=80000,
                           psram_free=7300000, psram_min=7290000, psram_largest=7200000, alloc_failed=0,
                           alloc_failed_size=0, alloc_failed_caps=0, tasks=8,
                           stacks={"main": 2500, "rep_executor": -1})
        self.health_step = {}

    def call(self, protocol, operation="STATUS", timeout=None):
        reply = super().call(protocol, operation, timeout)
        if protocol == "safe" and operation == "HEALTH":
            for key, step in self.health_step.items():
                self.health[key] += step
            reply = dict(reply, **self.health)
        return reply


def edge(second, mono_ns, width_ns=2_000_000, sync=1, at=5, wall_offset_ns=0):
    lo, hi = mono_ns - width_ns // 2, mono_ns + width_ns // 2
    return {"second": second, "sync": sync, "at": at, "mono": [lo, hi], "raw": [lo, hi],
            "wall": [lo + wall_offset_ns, hi + wall_offset_ns], "device": [lo, hi]}


class AnalysisTests(unittest.TestCase):
    def test_percentiles_nearest_rank(self):
        stats = endurance.percentiles(list(range(1, 101)))
        self.assertEqual((stats["min"], stats["median"], stats["p95"], stats["p99"], stats["max"]),
                         (1, 50, 95, 99, 100))
        self.assertEqual(endurance.percentiles([7])["p99"], 7)
        self.assertEqual(endurance.percentiles([]), {"count": 0})

    def test_slope_per_hour(self):
        self.assertAlmostEqual(endurance.slope_per_hour([0, 1800, 3600], [100, 90, 80]), -20)
        self.assertIsNone(endurance.slope_per_hour([5, 5], [1, 2]))
        self.assertIsNone(endurance.slope_per_hour([1], [1]))

    def test_task_stability_verdicts(self):
        self.assertEqual(endurance.task_stability([8] * 10, 2)["verdict"], "stable")
        self.assertEqual(endurance.task_stability([8, 8, 8, 9, 8, 9, 8, 8], 0)["verdict"], "bounded")
        self.assertEqual(endurance.task_stability([8, 8, 9, 9, 10, 10, 11, 11], 0)["verdict"], "growing")
        self.assertEqual(endurance.task_stability([8, 8, 8], 0)["verdict"], "insufficient")
        # The warm-up hides the lazy creation of rep_executor.
        self.assertEqual(endurance.task_stability([7, 8, 8, 8, 8, 8], 1)["verdict"], "stable")

    def test_burst_accounting(self):
        ui0 = {"injected": 10, "input_dropped": 0}
        ui1 = {"injected": 18, "input_dropped": 1}
        result = endurance.burst_accounting(ui0, ui1, {"ui_events": 5, "rejected": 2},
                                            {"ui_events": 9, "rejected": 6}, 9, 0)
        self.assertEqual((result["accepted"], result["delivered"], result["stale_rejected"], result["lost"]),
                         (8, 4, 4, 0))
        lost = endurance.burst_accounting(ui0, ui1, {"ui_events": 5, "rejected": 2},
                                          {"ui_events": 8, "rejected": 6}, 9, 0)
        self.assertEqual(lost["lost"], 1)

    def test_find_edge_brackets_the_second_change(self):
        t = [0.0]

        def probe():
            start = t[0]
            t[0] += 0.004
            rtc = 100 if start < 0.5 else 101
            ns = int(start * 1e9)
            return {"rtc": rtc, "sync": 1, "at": 2, "mb": ns // 1000, "ma": ns // 1000 + 500,
                    "host_mono_before_ns": ns, "host_mono_after_ns": int(t[0] * 1e9),
                    "host_wall_before_ns": ns, "host_wall_after_ns": int(t[0] * 1e9)}

        result = endurance.find_edge(probe, lambda: t[0], 1.4)
        self.assertEqual(result["second"], 101)
        self.assertLessEqual(result["mono"][0], 500_000_000)
        self.assertGreaterEqual(result["mono"][1], 500_000_000)
        self.assertLess(result["mono"][1] - result["mono"][0], 10_000_000)

    def test_find_edge_reports_jump_and_timeout(self):
        values = iter([100, 102])
        sample = lambda: {"rtc": next(values), "sync": 1, "at": 2}  # noqa: E731
        self.assertIn("jumped", endurance.find_edge(sample, lambda: 0, 1)["error"])
        t = [0.0]

        def still():
            t[0] += 0.1
            return {"rtc": 5, "sync": 1, "at": 2}
        self.assertIn("no RTC second edge", endurance.find_edge(still, lambda: t[0], 1.4)["error"])

    def test_drift_fit_recovers_rate_and_bounds_contain_it(self):
        ppm = 20.0
        edges = [edge(1000 + k, int(k * 1e9 / (1 + ppm * 1e-6))) for k in range(0, 36000, 600)]
        fit = endurance.drift_fit(edges, "mono")
        self.assertEqual(fit["status"], "fitted")
        self.assertAlmostEqual(fit["estimate_ppm"], ppm, delta=0.05)
        self.assertLessEqual(fit["bounds_ppm"][0], ppm)
        self.assertGreaterEqual(fit["bounds_ppm"][1], ppm)
        self.assertLess(fit["bounds_ppm"][1] - fit["bounds_ppm"][0], 0.2)
        self.assertEqual(endurance.drift_fit(edges[:1], "mono")["status"], "insufficient")

    def test_rtc_analysis_splits_at_clock_set_and_skips_stepped_wall(self):
        edges = [edge(k * 60, int(k * 60e9)) for k in range(10)]
        edges += [edge(k * 60, int(k * 60e9), sync=2) for k in range(10, 13)]
        edges[5] = dict(edges[5], wall=[w + 50_000_000 for w in edges[5]["wall"]])
        for later in edges[6:10]:
            later["wall"] = [w + 50_000_000 for w in later["wall"]]
        edges.append({"error": "no RTC second edge within the poll limit"})
        result = endurance.rtc_analysis(edges)
        self.assertEqual((result["segments"], result["failed_captures"]), (2, 1))
        self.assertEqual(result["mono"]["edges"], 10)
        self.assertEqual(len(result["host_realtime_steps"]), 1)
        self.assertIn("skipped", result["wall"]["status"])

    def test_boot_ids_split_at_uptime_drop(self):
        self.assertEqual(endurance.boot_ids([5, None, 9, 2, 3, None, 1]), [0, 0, 0, 1, 1, 1, 2])
        self.assertEqual(endurance.boot_ids([]), [])

    def test_analysis_keeps_each_boot_after_a_planned_restart(self):
        """A restart at the end of a run must not discard the lifecycle series before it."""
        health = dict(internal_free=150000, internal_largest=80000, psram_free=7300000, internal_min=120000,
                      psram_min=7290000, alloc_failed=0, tasks=8, stacks={"main": 2500})
        samples = [dict(t_s=k, state="list", uptime_us=1000 + k, health=dict(health), ui={"lvgl_bytes": 0,
                        "input_latency_max_us": 0}) for k in range(12)]
        samples.append(dict(samples[0], t_s=20, state="home", uptime_us=5))
        runner = types.SimpleNamespace(
            counts={}, latency={"start_ms": {}, "close_ms": {}, "event_ms": [], "ui_input_ms": []},
            delivery={"bursts": []}, samples=samples, edges=[], args=types.SimpleNamespace(warmup=3),
            report={"steps": [], "host_time_sync": {}})
        runner.analyze_state = functools.partial(endurance.EnduranceRunner.analyze_state, runner)
        analysis = endurance.EnduranceRunner.analyze(runner)
        self.assertEqual(set(analysis["tasks"]), {"list (boot 0)", "home (boot 1)"})
        self.assertEqual(analysis["tasks"]["list (boot 0)"]["verdict"], "stable")
        self.assertEqual(analysis["memory"]["list (boot 0)"]["internal_free"]["verdict"], "stable")

    def test_untested_criterion_is_not_a_pass(self):
        report = {"preflight": {"status": PASS}, "steps": [{"status": PASS}],
                  "criteria_results": {"resets": PASS, "memory": "UNTESTED"}}
        self.assertEqual(endurance.outcome(report), "INCOMPLETE")
        report["criteria_results"]["memory"] = "NOT APPLICABLE"
        self.assertEqual(endurance.outcome(report), PASS)

    def test_evaluate_marks_missing_data_untested(self):
        monitoring = {"unexpected_resets": [], "faults": [], "lost": []}
        results = endurance.evaluate({"alloc_failed_growth": None, "memory": {}, "tasks": {}}, monitoring, [])
        self.assertEqual(results["memory"], "UNTESTED")
        self.assertEqual(results["tasks"], "UNTESTED")
        self.assertEqual(results["allocation"], "UNTESTED")
        self.assertEqual(results["delivery"], "UNTESTED")
        self.assertEqual(results["resets"], PASS)

    def test_evaluate_failures(self):
        monitoring = {"unexpected_resets": [{}], "faults": [], "lost": [{"step": "soak"}]}
        analysis = {"alloc_failed_growth": 2, "memory": {"home": {"internal_free": {"verdict": "leak"}}},
                    "tasks": {"home": {"verdict": "growing"}},
                    "delivery": {"checked": True, "lost": 1, "undelivered_spaced": 0}}
        steps = [{"details": {"problems": ["delivery: round 1: lost"]}}]
        results = endurance.evaluate(analysis, monitoring, steps)
        self.assertEqual(set(results.values()), {FAIL, PASS})
        self.assertEqual(results["integrity"], PASS)  # delivery problems count under delivery
        for name in ("resets", "allocation", "memory", "tasks", "delivery", "communication"):
            self.assertEqual(results[name], FAIL, name)


class SoakTests(RunnerTestCase):
    def run_endurance(self, *argv, fake=None):
        def factory(path, monitor):
            fake.path = path
            fake.monitor = monitor
            return fake

        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = endurance.main(["--out", str(self.out), "--serial", SERIAL, "--level", "soak",
                                   "--rtc-every", "0", "--batch-every", "0", "--interval", "5", *argv],
                                  device_factory=factory, by_id=self.by_id,
                                  sleep=self.clock.sleep, now=self.clock.now)
        return code, json.loads((self.out / "report.json").read_text())

    def health_device(self, hook=None):
        self.fake = HealthDevice(self.clock, hook=hook)
        return self.fake

    def telemetry(self):
        return [json.loads(line) for line in (self.out / "telemetry.jsonl").read_text().splitlines()]

    def test_steady_soak_passes_with_telemetry(self):
        code, report = self.run_endurance("--hours", "0.02", fake=self.health_device())
        self.assertEqual(code, 0, report["criteria_results"])
        self.assertEqual(report["outcome"], PASS)
        self.assertGreaterEqual(len(self.telemetry()), 14)
        self.assertEqual(report["analysis"]["tasks"]["home"]["verdict"], "stable")
        self.assertEqual(report["analysis"]["memory"]["home"]["internal_free"]["verdict"], "stable")
        self.assertEqual(report["criteria_results"]["delivery"], "NOT APPLICABLE")
        self.assertTrue((self.out / "report.md").read_text().startswith("# M8.1 endurance"))

    def test_heap_regression_fails_memory(self):
        fake = self.health_device()
        fake.health_step = {"internal_free": -3000}
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(report["criteria_results"]["memory"], FAIL)
        self.assertEqual(report["outcome"], FAIL)
        self.assertEqual(code, 1)

    def test_single_heap_drop_is_not_a_leak(self):
        fake = self.health_device()

        def drop_once(device, protocol, operation):
            if protocol == "safe" and operation == "HEALTH" and device.count("safe") == 8:
                device.health["internal_free"] -= 20000
        fake.hook = drop_once
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(report["criteria_results"]["memory"], PASS)

    def test_task_growth_fails(self):
        fake = self.health_device()
        fake.health_step = {"tasks": 1}
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(report["analysis"]["tasks"]["home"]["verdict"], "growing")
        self.assertEqual(report["criteria_results"]["tasks"], FAIL)

    def test_failed_allocation_fails(self):
        fake = self.health_device()

        def fail_once(device, protocol, operation):
            if protocol == "safe" and operation == "HEALTH" and device.count("safe") == 6:
                device.health["alloc_failed"] += 1
        fake.hook = fail_once
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(report["criteria_results"]["allocation"], FAIL)

    def test_missing_health_blocks_baseline_and_leaves_checks_untested(self):
        code, report = self.run_endurance("--hours", "0.01", fake=self.device())
        self.assertEqual(self.step(report, "baseline")["status"], "BLOCKED")
        self.assertEqual(report["criteria_results"]["tasks"], "UNTESTED")
        self.assertEqual(report["outcome"], "INCOMPLETE")
        self.assertIsNone(self.telemetry()[0]["health"])

    def test_unexpected_reset_fails(self):
        fake = self.health_device()
        fake.reset_on_clock_call = 6
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(len(report["monitoring"]["unexpected_resets"]), 1)
        self.assertEqual(report["criteria_results"]["resets"], FAIL)
        self.assertEqual(report["outcome"], FAIL)

    def test_usb_loss_with_reconnect_is_survived(self):
        fake = self.health_device()

        def drop(device, protocol, operation):
            if protocol == "safe" and operation == "HEALTH" and device.count("safe") == 6:
                device.down_until = device.clock.t + 8.0
        fake.hook = drop
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(len(report["monitoring"]["reconnects"]), 1)
        self.assertEqual(report["criteria_results"]["communication"], PASS)
        self.assertEqual(report["outcome"], PASS)

    def test_usb_loss_without_return_fails_communication(self):
        fake = self.health_device()

        def gone(device, protocol, operation):
            if protocol == "safe" and operation == "HEALTH" and device.count("safe") == 6:
                device.down_until = 1e9
        fake.hook = gone
        code, report = self.run_endurance("--hours", "0.02", fake=fake)
        self.assertEqual(report["criteria_results"]["communication"], FAIL)
        self.assertEqual(report["outcome"], FAIL)

    def test_interrupt_keeps_partial_report(self):
        fake = self.health_device()

        def interrupt(device, protocol, operation):
            if protocol == "safe" and device.count("safe") == 7:
                raise KeyboardInterrupt
        fake.hook = interrupt
        code, report = self.run_endurance("--hours", "1", fake=fake)
        self.assertEqual(code, 130)
        self.assertTrue(report["interrupted"])
        self.assertEqual(report["outcome"], "INCOMPLETE")
        self.assertIsNotNone(report["finished"])
        self.assertGreaterEqual(len(self.telemetry()), 3)

    def test_log_limit_stops_early(self):
        code, report = self.run_endurance("--hours", "1", "--max-log-mb", "0.001", fake=self.health_device())
        self.assertIn("--max-log-mb", report["stopped"])
        self.assertEqual(report["outcome"], "INCOMPLETE")
        self.assertLess(self.clock.t, 3600)

    def test_no_device_blocks(self):
        for link in list(self.by_id.iterdir()):
            link.unlink()
        code, report = self.run_endurance("--hours", "0.01", fake=self.health_device())
        self.assertEqual((code, report["outcome"]), (2, "BLOCKED"))
        self.assertEqual(self.fake.calls, [])

    def test_argument_limits(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for argv in (["--level", "soak", "--hours", "30"], ["--level", "stress", "--cycles", "5000"],
                         ["--level", "soak", "--interval", "1"], ["--level", "soak", "--rtc-every", "2"]):
                with self.assertRaises(SystemExit):
                    endurance.parse_args(argv)
        args = endurance.parse_args(["--level", "stress"])
        self.assertEqual((args.cycles, args.ui_rounds, args.rtc_every), (100, 30, 0.0))


if __name__ == "__main__":
    unittest.main()
