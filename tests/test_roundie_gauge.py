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

    def test_only_bounded_operations_are_sent(self):
        for bad in ("CAPACITY", "CAPACITY 600 1", "CAPACITY -1", "capacity 600", "STATUS\n!RS1", "CAPACITY 123456",
                    "LEARN HOLD 0", "LEARN HOLD 12345", "LEARN HOLD", "LEARN GO", "learn"):
            with self.assertRaises(DeviceError):
                tool.request(None, bad)

    def test_cli_bounds_capacity(self):
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "50"])
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "7000"])
        for minutes in ("0", "1441"):
            with self.assertRaises(SystemExit):
                tool.main(["--port", "/dev/null", "learn-hold", minutes])


if __name__ == "__main__":
    unittest.main()
