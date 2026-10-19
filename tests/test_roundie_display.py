import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import roundie_display as display


class DisplayDiagnosticsTests(unittest.TestCase):
    def test_bounded_monotonic_trace_and_validation(self):
        summary = [12] + [0] * (len(display.SUMMARY) - 1)
        trace = [1, 5000000000] + [0] * (len(display.TRACE) - 2)
        response = dict(format=1, oldest=1, latest=1, summary=summary, trace=trace)
        with patch.object(display.secrets, 'token_hex', return_value='12345678'), \
             patch.object(display, 'write_command'), patch.object(display, 'read_serial_reply') as read:
            def reply(value):
                read.return_value = b'!RP1 12345678 0 ' + json.dumps(value).encode() + b'\n'
            reply(response)
            self.assertEqual(display.request(None, 'PERF 0')['trace']['at_us'], 5000000000)
            for change in (dict(format=2), dict(trace=[1]), dict(summary=[4]), dict(latest=True),
                           dict(trace=[True] + trace[1:]), dict(summary=[13] + summary[1:])):
                reply(dict(response, **change))
                with self.assertRaises(display.TransportError):
                    display.request(None)

    def test_only_bounded_operations(self):
        for operation in ('PERF -1', 'PERF 4294967296', 'BATCH 0', 'BATCH 13', 'RESET', 'PERF 0 extra'):
            with self.assertRaises(display.DeviceError):
                display.request(None, operation)
