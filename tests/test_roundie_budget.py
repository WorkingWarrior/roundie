"""RE1 BUDGET host client: parsing, validation and pre-M9 compatibility."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_budget as tool
from roundie_time import DeviceError, TransportError


class Port:
    def __init__(self, reply):
        self.data = bytearray(reply)
        self.written = b""

    def write(self, data):
        self.written += data
        return len(data)

    def flush(self):
        pass

    def read(self, size=1):
        chunk = bytes(self.data[:size])
        del self.data[:size]
        return chunk


INSTANCE = "42,9,1,2,3,2,1,150000,60000,8,2,1,3,4"
BOOT = "42,90,10,20,30,20,10,150000,60000,8,2,1,3,4294967295"


def ask(reply):
    port = Port(reply)
    with patch.object(tool.secrets, "token_hex", return_value="1234abcd"):
        return tool.request(port, timeout=1), port


class BudgetTests(unittest.TestCase):
    def test_reply_is_parsed_in_field_order(self):
        result, port = ask(f'log line\n!RE1 1234abcd 0 {{"budget":[{INSTANCE}],"boot":[{BOOT}]}}\n'.encode())
        self.assertIn(b"!RE1 1234abcd BUDGET", port.written)
        self.assertEqual(result["instance"]["near"], 2)
        self.assertEqual(result["instance"]["over"], 1)
        self.assertEqual(result["instance"]["callback_max_us"], 150000)
        self.assertEqual(result["boot"]["discarded"], 4294967295)
        self.assertEqual(list(result["boot"]), tool.FIELDS)

    def test_firmware_without_budget_is_reported_as_unsupported(self):
        status = '{"permitted":1,' + ",".join(f'"k{i}":{i}' for i in range(150)) + "}"
        with self.assertRaises(tool.Unsupported):
            ask(f"!RE1 1234abcd 258 {status}\n".encode())

    def test_malformed_counters_are_rejected(self):
        for budget in ("1,2", INSTANCE.replace("42", "-1"), INSTANCE.replace(",8,", ",9,")):
            with self.subTest(budget=budget), self.assertRaises(TransportError):
                ask(f'!RE1 1234abcd 0 {{"budget":[{budget}],"boot":[{BOOT}]}}\n'.encode())

    def test_only_the_read_only_operation_is_sent(self):
        with self.assertRaises(DeviceError):
            tool.request(Port(b""), "RESET")


if __name__ == "__main__":
    unittest.main()
