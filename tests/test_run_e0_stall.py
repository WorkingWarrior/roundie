import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "hardware_validation"))
import run_e0_stall as stall  # noqa: E402
from roundie_time import TransportError  # noqa: E402


class FakePort:
    """Answers one written command with a canned payload under its identity."""

    def __init__(self, code, payload, protocol="!RP1"):
        self.code = code
        self.payload = payload
        self.protocol = protocol
        self.out = b""
        self.written = b""

    def write(self, data):
        self.written += data
        identity = data.split()[1].decode()
        self.out = f"log\n{self.protocol} {identity} {self.code} {json.dumps(self.payload)}\n".encode()
        return len(data)

    def read(self, size=1):
        byte, self.out = self.out[:size], self.out[size:]
        return byte


def x_payload(kind="XREAD"):
    return {"format": 1, "fsbench": kind, "kib": 1024, "chunk": 4096, "yield": 0, "core": -1,
            "io_us": 200000, "sha_us": 30000, "total_us": 234000, "chunk_max_us": 1500,
            "on_core": [100, 156], "migrations": 3, "mmap_remain": 0, "probes_armed": 1,
            "gui": [20, 35000, 2000], "probes": [[0, 1, 20, 12000], [1, 1, 2, 230000],
                                                  [0, 20, 23, 10100], [1, 20, 23, 10200]],
            "capacity": [1, 9240576, 163840], "sha256": "ab"}


class XbenchTest(unittest.TestCase):
    def test_reply_and_row(self):
        port = FakePort(0, x_payload())
        reply = stall.xbench_request(port, "XREAD 1024 4096 0 W", 1.0)
        self.assertIn(b"FSBENCH XREAD 1024 4096 0 W", port.written)
        row = stall.stall_row("XREAD 1024 4096 0 W", reply)
        self.assertEqual(row["probe_gap_ms"], {"c0p1": 12.0, "c1p1": 230.0, "c0p20": 10.1, "c1p20": 10.2})
        self.assertEqual(row["gui_gap_ms"], 35.0)
        self.assertEqual(row["code"], 0)

    def test_invalid_operation_not_sent(self):
        port = FakePort(0, x_payload())
        for operation in ("XREAD 2048 4096 0 W", "XERASE 64 4096 0 W", "XREAD 64 4096 0 C2"):
            with self.assertRaises(TransportError):
                stall.xbench_request(port, operation, 1.0)
        self.assertEqual(port.written, b"")

    def test_wrong_kind_or_arrays_rejected(self):
        with self.assertRaises(TransportError):
            stall.xbench_request(FakePort(0, x_payload("XWRITE")), "XREAD 64 4096 0 W", 1.0)
        payload = x_payload()
        payload["probes"] = payload["probes"][:3]
        with self.assertRaises(TransportError):
            stall.xbench_request(FakePort(0, payload), "XREAD 64 4096 0 W", 1.0)


class ScriptPort(FakePort):
    """Answers by operation: XRESULT reports 259 `pending` times, then a result."""

    def __init__(self, pending, start_code=0):
        super().__init__(0, {})
        self.pending = pending
        self.start_code = start_code
        self.operations = []

    def write(self, data):
        parts = data.split()
        protocol, identity, operation = parts[0].decode(), parts[1].decode(), b" ".join(parts[2:]).decode()
        self.operations.append(operation)
        if operation.startswith("FSBENCH X") and operation != "FSBENCH XRESULT":
            code, payload = self.start_code, {"format": 1, "started": 1}
        elif operation == "FSBENCH XRESULT":
            if self.pending > 0:
                self.pending -= 1
                code, payload = 259, {"format": 1}
            else:
                code, payload = 0, x_payload("XWRITE")
        else:
            code, payload = 0, {"format": 1}
        self.out = f"{protocol} {identity} {code} {json.dumps(payload)}\n".encode()
        return len(data)


class LowSessionTest(unittest.TestCase):
    def test_polls_until_result(self):
        port = ScriptPort(pending=2)
        result = stall.low_session(port, "XWRITE 512 4096 0 L0", timeout=5.0)
        self.assertEqual(result["code"], 0)
        self.assertEqual(result["worker_reply_s"]["XRESULT"]["n"], 3)
        self.assertEqual(result["worker_reply_s"]["STATUS"]["n"], 3)
        self.assertEqual(port.operations[:4], ["FSBENCH XWRITE 512 4096 0 L0", "STATUS", "LIST", "FSBENCH XRESULT"])

    def test_refused_start_and_invalid(self):
        self.assertEqual(stall.low_session(ScriptPort(0, start_code=259), "XREAD 1024 4096 0 L", 1.0)["code"], 259)
        with self.assertRaises(TransportError):
            stall.low_session(ScriptPort(0), "XREAD 1024 4096 0 W", 1.0)

    def test_wrong_kind(self):
        with self.assertRaises(TransportError):
            stall.low_session(ScriptPort(0), "XREAD 1024 4096 0 L", 1.0)


class HistoryTest(unittest.TestCase):
    def test_entries(self):
        payload = {"history": {"state": "ok", "boots": 5, "brownouts": 1,
                               "entries": [[5, 3, 3950], [4, 9, 0]]}}
        history = stall.history_request(FakePort(0, payload, "!RS1"), timeout=1.0)
        self.assertEqual(history["brownouts"], 1)
        self.assertEqual(history["entries"][1], [4, 9, 0])

    def test_malformed_entry(self):
        payload = {"history": {"state": "ok", "boots": 1, "brownouts": 0, "entries": [[1, 3]]}}
        with self.assertRaises(TransportError):
            stall.history_request(FakePort(0, payload, "!RS1"), timeout=1.0)


class RaceTest(unittest.TestCase):
    def test_elfdelay(self):
        port = FakePort(0, {"format": 1, "elfdelay": 300})
        self.assertEqual(stall.elfdelay_request(port, "ELFDELAY 300", 1.0)["elfdelay"], 300)
        with self.assertRaises(TransportError):
            stall.elfdelay_request(port, "ELFDELAY -1", 1.0)

    def test_verdict(self):
        self.assertEqual(stall.race_verdict({"state": "RESULT", "result": [0, -1, -24]}),
                         {"outcome": "result", "result": [0, -1, -24]})
        self.assertEqual(stall.race_verdict({"state": "HOME"}), {"outcome": "home"})


if __name__ == "__main__":
    unittest.main()
