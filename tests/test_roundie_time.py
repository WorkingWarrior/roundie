"""Host utility tests; fake serial is protocol evidence, not physical I2C evidence."""

import importlib.util
from pathlib import Path
import time
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

SPEC = importlib.util.spec_from_file_location(
    "roundie_time", Path(__file__).resolve().parents[1] / "tools/roundie_time.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


def reply(request_id="1234abcd", code="OK", state=0, os_flag=0,
          rtc=1792314000, wall=1792314000, error=0):
    return (f"!RT1 {request_id} {code} state={state} os={os_flag} "
            f"rtc={rtc} wall={wall} err={error}\n").encode()


class FakeSerial:
    def __init__(self, mode="valid"):
        self.mode = mode
        self.incoming = bytearray()
        self.writes = []

    def write(self, wire):
        self.writes.append(wire)
        command = wire.strip().split()
        request_id = command[1].decode()
        now = int(time.time())
        if self.mode == "valid":
            self.incoming.extend(b"I (12) roundie: diagnostic log\r\n")
            self.incoming.extend(reply(request_id="ffffffff"))
            self.incoming.extend(reply(request_id=request_id, rtc=now, wall=now))
        elif self.mode == "short":
            return len(wire) - 1
        elif self.mode == "disconnected":
            raise OSError("device disconnected")
        elif self.mode == "malformed":
            self.incoming.extend(f"!RT1 {request_id} OK nonsense\n".encode())
        elif self.mode == "truncated":
            self.incoming.extend(f"!RT1 {request_id} OK state=0".encode())
        elif self.mode == "oversized":
            self.incoming.extend(f"!RT1 {request_id} ".encode() + b"X" * 300 + b"\n")
        elif self.mode == "i2c":
            self.incoming.extend(reply(request_id=request_id, code="RTC_READ", state=2,
                                       os_flag=-1, rtc=-1, wall=-1, error=263))
        return len(wire)

    def read(self, count):
        if self.incoming:
            byte = bytes(self.incoming[:count])
            del self.incoming[:count]
            return byte
        time.sleep(0.001)
        return b""


class TimeToolTests(unittest.TestCase):
    def test_valid_reply_and_crlf(self):
        self.assertEqual(tool.parse_reply(reply(), "1234abcd")["rtc"], 1792314000)
        self.assertIsNotNone(tool.parse_reply(reply().replace(b"\n", b"\r\n"), "1234abcd"))

    def test_logs_and_unrelated_ids(self):
        self.assertIsNone(tool.parse_reply(b"RTC good\n", "1234abcd"))
        self.assertIsNone(tool.parse_reply(reply(), "abcdef12"))

    def test_malformed_and_truncated_replies(self):
        for line in (reply()[:-1], reply().replace(b"state=0", b"state=9"),
                     reply() + b"extra", reply().replace(b"rtc=1792314000", b"rtc=garbage")):
            with self.subTest(line=line), self.assertRaises(tool.TransportError):
                tool.parse_reply(line, "1234abcd")

    def test_inconsistent_and_out_of_range(self):
        for line in (reply(os_flag=1), reply(rtc=4102444800), reply(state=3),
                     reply(state=2, os_flag=0, rtc=-1, wall=-1), reply(os_flag=-1)):
            with self.subTest(line=line), self.assertRaises(tool.TransportError):
                tool.parse_reply(line, "1234abcd")

    def test_os_failure_is_device_error(self):
        status = tool.parse_reply(reply(code="RTC_INVALID", state=3, os_flag=1,
                                       rtc=-1, wall=-1, error=264), "1234abcd")
        with self.assertRaises(tool.DeviceError):
            tool.require_valid(status)

    def test_readback_tolerances_and_second_boundary(self):
        status = tool.parse_reply(reply(), "1234abcd")
        status["round_trip"] = 0.05
        self.assertAlmostEqual(tool.require_valid(status, 1792314000.99), -0.99)
        tool.require_valid(status, 1792314003)
        with self.assertRaises(tool.DeviceError):
            tool.require_valid(status, 1792314003.01)
        status["wall"] += 2
        with self.assertRaises(tool.DeviceError):
            tool.require_valid(status, 1792314000)
        status["wall"] -= 2
        status["round_trip"] = 3.01
        with self.assertRaises(tool.DeviceError):
            tool.require_valid(status, 1792314000)

    def test_correlated_request(self):
        port = FakeSerial()
        status = tool.request(port, "GET", timeout=0.2)
        tool.require_valid(status)
        self.assertTrue(port.writes[0].startswith(b"\n!RT1 "))
        self.assertTrue(port.writes[0].endswith(b" GET\n"))

    def test_set_timestamp(self):
        port = FakeSerial()
        status = tool.request(port, "SET", timeout=0.2)
        timestamp = int(port.writes[0].strip().split()[-1])
        self.assertEqual(timestamp, status["sent"])
        self.assertLess(abs(timestamp - time.time()), 2)

    def test_timeout_disconnect_and_bad_frames(self):
        for mode in ("silent", "short", "disconnected", "malformed", "truncated", "oversized"):
            with self.subTest(mode=mode), self.assertRaises(tool.TransportError):
                tool.request(FakeSerial(mode), "GET", timeout=0.02)

    def test_transient_i2c_response_and_recovery(self):
        port = FakeSerial("i2c")
        status = tool.request(port, "GET", timeout=0.2)
        self.assertEqual(status["error"], 263)
        with self.assertRaises(tool.DeviceError):
            tool.require_valid(status)
        port.mode = "valid"
        tool.require_valid(tool.request(port, "GET", timeout=0.2))

    def test_permissions_exit_code(self):
        with patch.object(tool, "open_serial", side_effect=tool.TransportError("permission denied")):
            self.assertEqual(tool.main(["--port", "/dev/denied", "verify"]), 2)

    def test_no_reset_on_open(self):
        port = MagicMock()
        serial_module = SimpleNamespace(Serial=MagicMock(return_value=port), SerialException=OSError)
        def opened():
            self.assertIs(port.dtr, True)
            self.assertIs(port.rts, True)
            self.assertEqual(port.port, "/dev/test")
        port.open.side_effect = opened
        with patch.dict("sys.modules", {"serial": serial_module}):
            self.assertIs(tool.open_serial("/dev/test"), port)
        self.assertEqual(serial_module.Serial.call_args.kwargs["write_timeout"], 0.5)
        self.assertTrue(serial_module.Serial.call_args.kwargs["exclusive"])

    def test_restart_reconnect(self):
        context = MagicMock()
        before = {"rtc": 100}
        after = {"rtc": 103}
        with patch.object(tool, "open_serial", side_effect=[context, tool.TransportError("disconnected"), context]), \
             patch.object(tool, "request", side_effect=[before, before, after]) as requests, \
             patch.object(tool, "require_valid"), patch.object(tool, "show_status"), \
             patch.object(tool.time, "sleep"), \
             patch.object(tool.time, "monotonic", side_effect=[1, 1, 1, 2, 4]):
            tool.restart_verify("/dev/test")
            self.assertEqual([call.args[1] for call in requests.call_args_list],
                             ["GET", "RESTART", "GET"])

    def test_restart_deadline(self):
        context = MagicMock()
        with patch.object(tool, "open_serial", return_value=context), \
             patch.object(tool, "request", return_value={"rtc": 100}), \
             patch.object(tool, "require_valid"), \
             patch.object(tool.time, "monotonic", side_effect=[1, 1, 17]):
            with self.assertRaises(tool.TransportError):
                tool.restart_verify("/dev/test")

    def test_cli_device_error_exit_code(self):
        with patch.object(tool, "open_serial", return_value=MagicMock()), \
             patch.object(tool, "request", side_effect=tool.DeviceError("OS=1")):
            self.assertEqual(tool.main(["--port", "/dev/test", "verify"]), 3)


if __name__ == "__main__":
    unittest.main()
