import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_elf as elf


def status():
    return dict(allowed=1, busy=0, session="00000000", received=0, total=0,
                state=0, instance=1, completed=1, abi=1, loader="1.3.5", code=256,
                data=115, alloc=0, alloc_peak=64, calls=6, load_us=1000,
                execute_us=2000, unload_us=100, loader_result=0, app_result=42,
                executed=1, over_budget=0, error="",
                before=[1, 1, 1, 1, 1], relocated=[1, 1, 1, 1, 1],
                during=[1, 1, 1, 1, 1], after=[1, 1, 1, 1, 1])


class ElfToolTests(unittest.TestCase):
    def test_lifecycle_completion_and_no_retry_on_lost_ack(self):
        active = status()
        active.update(abi=2, state=3, busy=1, control=[1, 0, 0])
        stopped = dict(active, state=11, control=[1, 1, 0])
        with patch.object(elf, "request", side_effect=[active, stopped]) as request:
            self.assertEqual(elf.control(object(), "STOP", 1)["state"], 11)
            self.assertEqual([c.args[1] for c in request.call_args_list], ["STOP 1", "STATUS"])
        with patch.object(elf, "request", side_effect=elf.TransportError("lost ack")) as request:
            with self.assertRaises(elf.TransportError):
                elf.control(object(), "UNLOAD", 1)
            self.assertEqual(request.call_count, 1)

    def test_addressed_response_and_logs(self):
        data = status()
        line = b"!RE1 12345678 0 " + json.dumps(data).encode() + b"\n"
        self.assertEqual(elf.parse_reply(line, "12345678"), data)
        self.assertIsNone(elf.parse_reply(line, "87654321"))
        self.assertIsNone(elf.parse_reply(b"application exited successfully\n", "12345678"))

    def test_malformed_and_device_refusal(self):
        for key, value in (("busy", 2), ("state", 12), ("total", 65537),
                           ("before", []), ("app_result", "42"), ("session", "../file")):
            with self.subTest(key=key):
                data = status()
                data[key] = value
                line = b"!RE1 12345678 0 " + json.dumps(data).encode() + b"\n"
                with self.assertRaises(elf.TransportError):
                    elf.parse_reply(line, "12345678")
        line = b"!RE1 12345678 259 " + json.dumps(status()).encode() + b"\n"
        with self.assertRaises(elf.DeviceError):
            elf.parse_reply(line, "12345678")

    def test_actual_result_and_instance(self):
        with patch.object(elf, "request", side_effect=[status(), status()]) as request:
            self.assertEqual(elf.run(object(), "a" * 64)["app_result"], 42)
            self.assertEqual(request.call_count, 2)
        changed = status()
        changed["instance"] = 2
        with patch.object(elf, "request", side_effect=[status(), changed]):
            with self.assertRaisesRegex(elf.DeviceError, "identity changed"):
                elf.run(object(), "a" * 64)

    def test_timeout_never_unloads(self):
        active = status()
        active["state"] = 3
        active["busy"] = 1
        with patch.object(elf, "request", return_value=active) as request, \
             patch.object(elf.time, "monotonic", side_effect=[0, 5]):
            with self.assertRaisesRegex(elf.DeviceError, "no forced unload"):
                elf.run(object(), "a" * 64)
            self.assertEqual([call.args[1] for call in request.call_args_list], ["RUN " + "a" * 64, "STATUS"])

    def test_lost_begin_ack_aborts_without_data_retry(self):
        with patch.object(elf, "request", side_effect=[status(), elf.TransportError("lost ack"), status()]) as request:
            with self.assertRaises(elf.TransportError):
                elf.upload(object(), "unused", dict(size=100, sha256="a" * 64))
            operations = [call.args[1].split()[0] for call in request.call_args_list]
            self.assertEqual(operations, ["STATUS", "BEGIN", "ABORT"])


if __name__ == "__main__":
    unittest.main()
