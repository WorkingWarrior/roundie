import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_gauge as tool
from roundie_time import DeviceError, TransportError


def status(**changes):
    value = dict(format=1, step=0, design_mah=3000, dm_full_mah=3000, dm_design_mah=3000,
                 battery_flags=0x5028, operation_flags=0x00a4, state=0, voltage_mv=4124, soc=20,
                 remaining_mah=593, full_mah=3000, current_ma=0, initialized=1)
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
        for bad in (reply()[:-1], b"!RG1 1234abcd 0 {}\n", reply(status(format=2)),
                    reply(status(design_mah=65536)), reply(status(dm_full_mah=-1)),
                    reply(status(step=8)), reply(status(current_ma=40000)),
                    reply(status(initialized=True)), reply(status(soc="20")),
                    b"!RG1 1234abcd x {}\n", reply() + b"x" * 600):
            with self.assertRaises(TransportError, msg=bad):
                tool.parse_reply(bad, "1234abcd")
        with self.assertRaisesRegex(DeviceError, "step write"):
            tool.parse_reply(reply(status(step=5), code=259), "1234abcd")

    def test_only_bounded_operations_are_sent(self):
        for bad in ("CAPACITY", "CAPACITY 600 1", "CAPACITY -1", "capacity 600", "STATUS\n!RS1", "CAPACITY 123456"):
            with self.assertRaises(DeviceError):
                tool.request(None, bad)

    def test_cli_bounds_capacity(self):
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "50"])
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/null", "capacity", "7000"])


if __name__ == "__main__":
    unittest.main()
