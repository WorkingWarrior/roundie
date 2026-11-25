import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_gauge as tool
from roundie_time import DeviceError, TransportError


def status(**changes):
    value = dict(format=2, step=0, written=0, design_mah=3000, dm_full_mah=3000, dm_design_mah=3000,
                 gauging_config=0x102a, design_voltage_mv=3700, charging_voltage_mv=4200,
                 taper_current_ma=100, near_full_mah=200, battery_flags=0x5028, operation_flags=0x00a4, state=0, voltage_mv=4124, soc=20,
                 remaining_mah=593, full_mah=3000, current_ma=0, initialized=1)
    value.update(changes)
    return value


def learn(**changes):
    value = {"learn": 1, "phase": "qualified", "result": "none", "cause": "none", "hold": "running"}
    for key in tool.LEARN_INTS:
        value[key] = 0
    value.update(changes)
    return value


def reply(state=None, code=0, request_id="1234abcd"):
    return f"!RG1 {request_id} {code} {json.dumps(state or status())}\n".encode()


class GaugeToolTests(unittest.TestCase):
    def test_addressing_schema_and_failure(self):
        parsed = tool.parse_reply(reply(), "1234abcd")
        self.assertEqual(parsed["dm_full_mah"], 3000)
        self.assertEqual(parsed["step_name"], "none")
        self.assertIsNone(tool.parse_reply(reply(), "ffffffff"))
        for bad in (reply()[:-1], b"!RG1 1234abcd 0 {}\n", reply(status(format=1)), reply(status(written=2)),
                    reply(status(taper_current_ma=-1)), reply(status(near_full_mah=None)),
                    reply(status(design_mah=65536)), reply(status(dm_full_mah=-1)),
                    reply(status(step=8)), reply(status(current_ma=40000)),
                    reply(status(initialized=True)), reply(status(soc="20")),
                    b"!RG1 1234abcd x {}\n", reply() + b"x" * 1100):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(bad, "1234abcd")
        with self.assertRaisesRegex(DeviceError, "step write"):
            tool.parse_reply(reply(status(step=4), code=259), "1234abcd")
        with self.assertRaisesRegex(DeviceError, "step verify-cfg"):
            tool.parse_reply(reply(status(step=5, written=1), code=265), "1234abcd")
        self.assertEqual(tool.STEPS.index("enter"), 3)
        self.assertNotIn("full-access", tool.STEPS)

    def test_battery_reply(self):
        battery = {"format": 1, "board": "Waveshare ESP32-S3-Touch-AMOLED-1.75C", "display": [466, 466],
                   "state": 2, "error": 0, "present": 1, "voltage_valid": 1, "voltage_mv": 4100,
                   "soc_valid": 1, "soc": 87, "current_valid": 0, "current_ma": 0, "charging": 1,
                   "discharging": 0, "initialized": 0, "battery_flags": 0x2412}
        parsed = tool.parse_reply(reply(battery), "1234abcd")
        self.assertEqual(parsed["display"], [466, 466])
        self.assertEqual(parsed["charging"], 1)
        for bad in ({**battery, "format": 2}, {**battery, "display": [466]}, {**battery, "charging": 2},
                    {**battery, "board": ""}, {**battery, "current_ma": 40000}):
            with self.assertRaises(TransportError):
                tool.parse_reply(reply(bad), "1234abcd")
        with self.assertRaises(DeviceError):
            tool.parse_reply(reply(battery, code=259), "1234abcd")

    def test_format3_and_learn_reply(self):
        extra = dict(format=3, battery_low_centi=700, learning_low_temp=119, overload_ma=1500, reserve_mah=0,
                     flag_config_a=0x0c8c, flag_config_b=0x8c)
        self.assertEqual(tool.parse_reply(reply(status(**extra)), "1234abcd")["battery_low_centi"], 700)
        with self.assertRaises(TransportError):
            tool.parse_reply(reply(status(format=3)), "1234abcd")
        parsed = tool.parse_reply(reply(learn(edv2_ma=-90)), "1234abcd")
        self.assertEqual(parsed["phase"], "qualified")
        for bad in (learn(learn=2), learn(phase="x"), learn(cause="?"), learn(hold=None), learn(fcc="600")):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(reply(bad), "1234abcd")
        with self.assertRaisesRegex(DeviceError, "learning command"):
            tool.parse_reply(reply(learn(), code=258), "1234abcd")

    def test_log_reply_and_summary(self):
        # seq, time_ms, mv, ma, temp, rm, fcc, op, flags, hold, link, avg_ma
        raw = [[0, 1000, 4100, 0, 250, 580, 600, 0x10, 0, 1, 0x07, 0],      # USB, hold
               [1, 6000, 4090, -90, 251, 579, 600, 0x10, 0, 1, 0x05, -88],  # battery, hold
               [2, 11000, 4085, -60, 252, 578, 600, 0x10, 0, 1, 0x05, -80],
               [3, 16000, 4080, -20, 252, 578, 600, 0x10, 0, 8, 0x01, -40], # battery, released
               [4, 21000, 0, 0, 0, 0, 0, 0, 0, 8, 0x00, 0]]                  # invalid sample
        payload = {"log": 1, "usb_monitor": 1, "total": 5, "first": 0, "e": raw, "next": 5}
        parsed = tool.parse_reply(reply(payload), "1234abcd")
        self.assertEqual(parsed["e"][1]["ma"], -90)
        self.assertEqual(parsed["e"][3]["hold"], "expired")
        self.assertEqual(parsed["e"][3]["avg_ma"], -40)
        summary = tool.summarize_log(parsed["e"])
        self.assertEqual(summary["battery_samples"], 3)
        self.assertEqual(summary["usb_samples"], 1)
        self.assertEqual(summary["hold"]["discharge_ma"], {"n": 2, "avg": 75.0, "min": 60, "max": 90})
        self.assertEqual(summary["hold"]["at_least_required"], 1)
        self.assertEqual(summary["hold"]["rm_first"], 579)
        self.assertEqual(summary["hold"]["rm_last"], 578)
        self.assertEqual(summary["no_hold"]["discharge_ma"]["max"], 20)
        self.assertTrue(summary["vdq_seen"])
        self.assertFalse(summary["edv2_seen"])
        self.assertEqual(summary["gaps_ms_max"], 5000)
        self.assertEqual(summary["after_wake"]["n"], 0)
        self.assertIsNone(summary["battery_span"]["mean_ma_from_rm"])
        woke = [dict(parsed["e"][1], link=0x09, ma=-12, avg_ma=-9, time_ms=0, rm=579),
                dict(parsed["e"][1], link=0x09, ma=-14, avg_ma=-11, time_ms=1_800_000, rm=571)]
        summary = tool.summarize_log(woke)
        self.assertEqual(summary["after_wake"]["average_ma"], {"n": 2, "avg": 10.0, "min": 9, "max": 11})
        self.assertEqual(summary["battery_span"]["mean_ma_from_rm"], 16.0)
        for bad in ({**payload, "log": 2}, {**payload, "e": [[1, 2]]}, {**payload, "e": [[0] * 11 + ["x"]]},
                    {**payload, "e": [[0] * 11]},
                    {**payload, "total": -1}):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(reply(bad), "1234abcd")
        with self.assertRaisesRegex(DeviceError, "log command"):
            tool.parse_reply(reply(payload, code=258), "1234abcd")

    def test_end_reply(self):
        end = {"end": 1, "hold": "edv2", "alert": "finished", "hold_result": "none"}
        for key in tool.END_INTS:
            end[key] = 0
        end.update(snap=1, snap_mv=3560, snap_ma=-105, snap_avg_ma=-104, protect=1)
        self.assertEqual(tool.parse_reply(reply({**end, "hold": "test-end", "simulated": 1, "test": 1}),
                                          "1234abcd")["hold"], "test-end")
        parsed = tool.parse_reply(reply(end), "1234abcd")
        self.assertEqual(parsed["snap_mv"], 3560)
        # New hold states keep their log index after the old ones.
        self.assertEqual(tool.LEARN_NAMES["hold"].index("cancelled"), 9)
        self.assertEqual(tool.LEARN_NAMES["hold"][10:], ("edv2", "cold", "no-result", "test-end", "test-stop",
                                                          "test-usb"))
        for name in ("no-result", "cold"):
            self.assertEqual(tool.parse_reply(reply({**end, "hold": name, "alert": "stopped"}),
                                              "1234abcd")["hold"], name)
        done = {**end, "hold": "done", "hold_result": "updated", "learned": 1}
        self.assertEqual(tool.parse_reply(reply(done), "1234abcd")["learned"], 1)
        for bad in ({**end, "end": 2}, {**end, "alert": "x"}, {**end, "hold": "?"}, {**end, "snap_mv": "1"},
                    {**end, "learned": 1}, {**done, "hold_result": "unchanged"}, {**done, "simulated": 1}):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(reply(bad), "1234abcd")
        with self.assertRaisesRegex(DeviceError, "end command"):
            tool.parse_reply(reply(end, code=258), "1234abcd")

    def test_window_reply(self):
        def point(age_ms, rm, mv, sleeps, slept_ms, timer, other, awake_ms, gaps):
            return dict(zip(tool.POINT_INTS, (age_ms, rm, mv, sleeps, slept_ms, timer, other, awake_ms, gaps)))
        window = {"window": 1, "state": "closed", "hold": "test-end", "started": 1, "samples": 90, "vmin": 4080,
                  "vmax": 4110, "causes": (1 << 4) | (1 << 7), "protect": 1}
        points = {"s": point(1_260_000, 590, 4110, 3, 180_000, 2, 1, 40_000, 2),
                  "p": point(900_000, 584, 4095, 3, 180_000, 2, 1, 40_000, 2),
                  "l": point(60_000, 582, 4090, 15, 900_000, 13, 2, 125_000, 14)}
        for prefix, values in points.items():
            window.update({f"{prefix}_{key}": value for key, value in values.items()})
        parsed = tool.parse_reply(reply(window), "1234abcd")
        summary = tool.summarize_window(parsed)
        self.assertEqual(summary["wake_causes"], ["timer", "gpio"])
        protected = summary["protected"]
        self.assertEqual(protected["seconds"], 840.0)
        self.assertEqual((protected["light_sleeps"], protected["slept_s"], protected["mean_sleep_s"]), (12, 720.0, 60.0))
        self.assertEqual((protected["timer_wakes"], protected["other_wakes"]), (11, 1))
        self.assertAlmostEqual(protected["mean_awake_between_s"], 85.0 / 12, places=2)
        self.assertEqual(protected["rm_drop_mah"], 2)
        self.assertAlmostEqual(protected["mean_ma_from_rm"], 2 / (840 / 3600), places=1)
        self.assertEqual(protected["mean_ma_bounds"], [round(1 / (840 / 3600), 1), round(3 / (840 / 3600), 1)])
        self.assertEqual(summary["whole"]["rm_drop_mah"], 8)
        self.assertEqual(summary["before_protection"]["light_sleeps"], 0)
        self.assertIsNone(summary["before_protection"]["mean_sleep_s"])
        self.assertIn("not the light-sleep current", summary["note"])
        # Without the protect point only start and last are required.
        bare = {key: value for key, value in parsed.items() if not key.startswith("p_")}
        bare["protect"] = 0
        self.assertNotIn("protected", tool.summarize_window(tool.parse_reply(reply(bare), "1234abcd")))
        self.assertEqual(tool.summarize_window(tool.parse_reply(reply({**window, "state": "none"}), "1234abcd")),
                         {"state": "none"})
        for bad in ({**window, "window": 2}, {**window, "state": "x"}, {**window, "hold": "?"},
                    {**window, "l_rm": -1}, {**window, "samples": "1"}, {k: v for k, v in window.items() if k != "s_mv"}):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(reply(bad), "1234abcd")
        with self.assertRaisesRegex(DeviceError, "window command"):
            tool.parse_reply(reply(window, code=258), "1234abcd")

    def test_only_bounded_operations_are_sent(self):
        for bad in ("CAPACITY", "CAPACITY 600 1", "CAPACITY -1", "capacity 600", "STATUS\n!RS1", "CAPACITY 123456",
                    "LEARN HOLD 0", "LEARN HOLD 12345", "LEARN HOLD", "LEARN GO", "learn", "LEARN LOG",
                    "LEARN LOG -1", "LEARN LOG 12345678901", "LEARN END 1", "LEARN ENDX", "LEARN TEST END",
                    "LEARN TEST GO 5", "LEARN TEST END -1", "LEARN TEST END 12345", "LEARN TEST end 5",
                    "LEARN WINDOW 1", "LEARN WINDOWX"):
            with self.assertRaises(DeviceError):
                tool.request(None, bad)

    def test_cli_bounds_capacity(self):
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "50"])
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "7000"])
        for delay in ("-1", "3601"):
            with self.assertRaises(SystemExit):
                tool.main(["--port", "/dev/null", "learn-test", "end", delay])
        for minutes in ("0", "1441"):
            with self.assertRaises(SystemExit):
                tool.main(["--port", "/dev/null", "learn-hold", minutes])


if __name__ == "__main__":
    unittest.main()
