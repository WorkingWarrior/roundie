import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "hardware_validation"))
import run_e0_followup as followup  # noqa: E402
from roundie_time import TransportError  # noqa: E402


def heap(internal, psram):
    return {"internal_free": internal, "psram_free": psram}


class ProbeVerdictTest(unittest.TestCase):
    def test_detected_allocation(self):
        verdict = followup.probe_verdict("PSRAM", 4096, heap(1000, 50000), heap(1000, 45900), heap(1000, 50000))
        self.assertEqual(verdict["problems"], [])
        self.assertEqual(verdict["overhead"], 4)

    def test_blind_probe_fails(self):
        verdict = followup.probe_verdict("INTERNAL", 1024, heap(9000, 5), heap(9000, 5), heap(9000, 5))
        self.assertTrue(verdict["problems"])

    def test_wrong_pool_and_no_release_fail(self):
        verdict = followup.probe_verdict("INTERNAL", 1024, heap(9000, 9000), heap(7976, 6000), heap(7976, 9000))
        self.assertEqual(len(verdict["problems"]), 2)


class ClassifyTest(unittest.TestCase):
    def test_one_time_arena(self):
        after = [heap(864, 868)] + [heap(864 + d, 868 - d) for d in (0, 8, -8, 0)]
        result = followup.classify_cycles(heap(1000, 1000), after)
        self.assertEqual(result["class"], "arena")
        self.assertEqual(result["psram_free"]["first_cycle_drop"], 132)

    def test_leak(self):
        after = [heap(1000, 100000 - 200 * i) for i in range(20)]
        self.assertEqual(followup.classify_cycles(heap(1000, 100000), after)["class"], "leak")

    def test_slope(self):
        self.assertAlmostEqual(followup.linear_slope([0, 2, 4, 6]), 2.0)
        self.assertIsNone(followup.linear_slope([1]))


def trace(at, flushes=1, handler=10):
    return {"at_us": at, "flushes": flushes, "handler_us": handler, "render_us": handler, "dma_us": 1}


class FrameStatsTest(unittest.TestCase):
    def test_periods_only_between_flushing_iterations(self):
        stats = followup.frame_stats([trace(0), trace(50, flushes=0), trace(100), trace(300, handler=30)])
        self.assertEqual(stats["flushing"], 3)
        self.assertEqual(stats["period_us"], {"max": 200, "mean": 150.0})
        self.assertEqual(stats["handler_us"]["max"], 30)


class PairPort:
    """Answers the second command first, as a busy worker never would; the
    reader must match replies by identity, not by order."""

    def __init__(self):
        self.ids = []
        self.out = b""

    def write(self, data):
        parts = data.split()
        self.ids.append((parts[0].decode(), parts[1].decode()))
        if len(self.ids) == 2:
            (p1, i1), (p2, i2) = self.ids
            self.out = f"log\n{p2} {i2} 0 {{}}\n{p1} {i1} 0 {json.dumps({'total_us': 5})}\n".encode()
        return len(data)

    def read(self, size=1):
        byte, self.out = self.out[:size], self.out[size:]
        return byte


class TimedPairTest(unittest.TestCase):
    def test_matches_both_replies(self):
        timing, lines = followup.timed_pair(PairPort(), ("!RP1", "FSBENCH READ 1024"), ("!RS1", "STATUS"), 0)
        self.assertTrue(lines[0].startswith(b"!RP1 "))
        self.assertTrue(lines[1].startswith(b"!RS1 "))
        self.assertGreaterEqual(timing["second_reply_s"], 0)

    def test_missing_reply(self):
        class Silent(PairPort):
            def write(self, data):
                return len(data)
        with self.assertRaises(TransportError):
            followup.timed_pair(Silent(), ("!RP1", "X"), ("!RS1", "STATUS"), 0, timeout=0.05)


if __name__ == "__main__":
    unittest.main()
