import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import roundie_display as display


class DisplayScaleTests(unittest.TestCase):
    def test_strip_scaling_matches_full_frame(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            for standard in ('c11', 'c23'):
                output = str(Path(folder) / f'scale-{standard}')
                subprocess.run(['cc', f'-std={standard}', '-Wall', '-Wextra', '-Werror', '-O1',
                                '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                '-Imain', 'main/display_scale.c', 'tests/test_display_scale.c',
                                '-o', output], cwd=root, check=True, capture_output=True)
                subprocess.run([output], check=True, capture_output=True, timeout=60)


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
            for change in (dict(format=3), dict(trace=[1]), dict(summary=[4]), dict(latest=True),
                           dict(trace=[True] + trace[1:]), dict(summary=[13] + summary[1:])):
                reply(dict(response, **change))
                with self.assertRaises(display.TransportError):
                    display.request(None)
            reply(dict(response, format=2, trace=trace + [7] * 8))
            self.assertEqual(display.request(None)['trace']['swap_us'], 7)
            reply(dict(response, format=4, trace=trace + [7] * 8 + [9, 10, 11]))
            self.assertEqual(display.request(None)['trace']['chunks'], 11)
            reply(dict(response, format=4, trace=trace + [7] * 8))
            with self.assertRaises(display.TransportError):
                display.request(None)

    def test_only_bounded_operations(self):
        for operation in ('PERF -1', 'PERF 4294967296', 'BATCH 0', 'BATCH 13', 'RESET', 'PERF 0 extra'):
            with self.assertRaises(display.DeviceError):
                display.request(None, operation)

    def test_bench_reports_completed_frames_only(self):
        progress = [dict(format=3, active=1, frames=10),
                    dict(format=3, active=0, requested=30, frames=30, period_sum_us=900000,
                         handler_sum_us=600000)]
        with patch.object(display, 'request', side_effect=[{}] + progress), \
             patch.object(display.time, 'sleep'):
            result = display.bench(None, 30)
        self.assertEqual(result['fps'], 1000000 / 30000)
        incomplete = dict(progress[1], frames=29)
        with patch.object(display, 'request', side_effect=[{}, incomplete]), \
             patch.object(display.time, 'sleep'), self.assertRaises(display.DeviceError):
            display.bench(None, 30)

    def test_bench_rejects_wrong_format(self):
        with patch.object(display.secrets, 'token_hex', return_value='12345678'), \
             patch.object(display, 'write_command'), \
             patch.object(display, 'read_serial_reply', return_value=b'!RP1 12345678 0 {"format":2}\n'), \
             self.assertRaises(display.TransportError):
            display.request(None, 'BENCH')
