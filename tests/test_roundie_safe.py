import io
import json
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_safe as tool


def status():
    return dict(safe=1, retained=1, persistence=1, skip_gui=0, trial=0,
                test_build=0, restart=0, failures=3, fault_remaining=0,
                elf="a" * 64, other_elf="b" * 64, running="ota_1", reason="boot_loop")


def reply(state=None, code=0):
    return f"!RS1 1234abcd {code} {json.dumps(state or status())}\n".encode()


class SafeToolTests(unittest.TestCase):
    def test_addressing_schema_and_failure(self):
        self.assertEqual(tool.parse_reply(reply(), "1234abcd"), status())
        self.assertIsNone(tool.parse_reply(reply(), "ffffffff"))
        with_app = {**status(), "fault_app": "org.roundie.ui"}
        self.assertEqual(tool.parse_reply(reply(with_app), "1234abcd"), with_app)
        for bad in (reply()[:-1], b"!RS1 1234abcd 0 {}\n", reply({**status(), "safe": 2}),
                    reply({**status(), "failures": 4}), reply({**status(), "elf": "bad"}),
                    reply({**status(), "fault_app": 'bad"id'})):
            with self.subTest(bad=bad), self.assertRaises(tool.TransportError):
                tool.parse_reply(bad, "1234abcd")
        with self.assertRaises(tool.DeviceError):
            tool.parse_reply(reply(code=258), "1234abcd")

    def test_single_bounded_request_uses_shared_transport(self):
        class Port:
            incoming = bytearray(b"ordinary log\n" + reply())
            writes = 0
            def write(self, data):
                self.writes += 1
                assert data == b"\n!RS1 1234abcd STATUS\n"
                return len(data)
            def read(self, size):
                data = bytes(self.incoming[:size])
                del self.incoming[:size]
                return data
        port = Port()
        with patch.object(tool.secrets, "token_hex", return_value="1234abcd"):
            self.assertEqual(tool.request(port, "STATUS"), status())
        self.assertEqual(port.writes, 1)

    def test_timeout_never_retries_mutation(self):
        class Port:
            writes = 0
            def write(self, data):
                self.writes += 1
                return len(data)
            def read(self, size):
                time.sleep(0.001)
                return b""
        port = Port()
        with self.assertRaises(tool.TransportError):
            tool.request(port, "REQUEST", timeout=0.02)
        self.assertEqual(port.writes, 1)

    def test_cli_checks_identity_before_open(self):
        with patch.object(tool, "open_serial") as opened, patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(tool.main(["--port", "HOST_ONLY", "select", "ota_0", "--elf", "bad"]), 3)
            opened.assert_not_called()


if __name__ == "__main__":
    unittest.main()
