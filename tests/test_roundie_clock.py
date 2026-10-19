"""Read-only diagnostic protocol and interval-rate regression tests."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import roundie_clock as tool


def sample(at=0, offset=0):
    wall = 1792390000000000 + at * 1000000
    mono = 6000000000000 + at * 1000000
    return dict(state=0, os=0, rtc=1792390000 + at, err=0, valid=1,
                wb=wall, wa=wall + 2000, mb=mono, ma=mono + 2000,
                usable=1, lo=offset - 500000, hi=offset + 502000,
                sync=1, at=1000000, host_reference_usable=True,
                host_wall_before_ns=wall * 1000, host_mono_before_ns=mono * 1000)


def wire(s):
    return ("!RT1 1234abcd DIAG " + " ".join(f"{k}={s[k]}" for k in tool.FIELDS) + "\n").encode()


class ClockTests(unittest.TestCase):
    def test_production_c_logic_and_existing_rtc_protocol(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            for standard in ('c11', 'c23'):
                for name, sources in (
                    ('diagnostic', ['main/clock_diagnostic.c', 'tests/test_clock_diagnostic.c']),
                    ('codec', ['components/roundie_board/sensor_codec.c', 'tests/test_sensor_codec.c']),
                    ('protocol', ['main/time_protocol.c', 'components/roundie_board/sensor_codec.c',
                                  'tests/test_time_protocol.c'])):
                    output = str(Path(folder) / f'{name}-{standard}')
                    subprocess.run(['cc', f'-std={standard}', '-Wall', '-Wextra', '-Werror',
                                    '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                    '-Imain', '-Icomponents/roundie_board',
                                    '-Icomponents/roundie_board/include', *sources, '-o', output],
                                   cwd=root, check=True, capture_output=True)
                    subprocess.run([output], check=True, capture_output=True)

    def test_valid_reply_and_sign(self):
        s = sample()
        s.update(lo=-2000, hi=1000000)
        self.assertEqual(tool.parse_reply(wire(s), "1234abcd"),
                         {k: s[k] for k in tool.FIELDS})
        s['rtc'] -= 2
        s.update(lo=-2002000, hi=-1000000)
        self.assertLess(tool.parse_reply(wire(s), "1234abcd")['hi'], 0)

    def test_malformed_inconsistent_and_old_firmware(self):
        s = sample()
        for line in (wire(s), wire(s)[:-1], b"!RT1 1234abcd BAD_COMMAND state=0\n",
                     wire(s).replace(b"state=0", b"state=8"), wire(s) + b"extra"):
            with self.subTest(line=line), self.assertRaises(tool.TransportError):
                tool.parse_reply(line, "1234abcd")

    def test_unusable_rtc_and_failed_read(self):
        for state, os_flag, error in ((3, 1, 264), (3, 0, 264), (2, -1, 263)):
            s = sample()
            s.update(state=state, os=os_flag, err=error, usable=0, rtc=-1, lo=0, hi=0)
            self.assertFalse(tool.parse_reply(wire(s), "1234abcd")['usable'])
            self.assertEqual(tool.trend([s, sample(30)])['classification'], 'unavailable')

    def test_stable_offset_does_not_prove_drift(self):
        for offset in (0, -2000000, 2000000):
            result = tool.trend([sample(t, offset) for t in (0, 30, 120, 300)])
            self.assertEqual(result['classification'], 'unresolved')
            low, high = result['relative_rate_ppm_bounds']
            self.assertLess(low, 0)
            self.assertGreater(high, 0)

    def test_synthetic_known_drift_both_signs(self):
        for rate in (-10000, 10000):
            result = tool.trend([sample(t, t * rate) for t in (0, 30, 120, 300)])
            self.assertEqual(result['classification'], 'supported_trend')
            low, high = result['relative_rate_ppm_bounds']
            self.assertLess(low, rate)
            self.assertGreater(high, rate)

    def test_short_window_cannot_resolve_small_drift(self):
        result = tool.trend([sample(t, t * 20) for t in (0, 30, 120, 300)])
        self.assertEqual(result['classification'], 'unresolved')

    def test_discontinuities(self):
        for key, delta in (('sync', 1), ('at', 1), ('mb', -6000000000000),
                           ('wb', 1000000), ('host_wall_before_ns', 1000000000)):
            a, b = sample(), sample(300)
            b[key] += delta
            with self.subTest(key=key):
                self.assertEqual(tool.trend([a, b])['classification'], 'unavailable')
        a = sample()
        a['host_reference_usable'] = False
        self.assertEqual(tool.trend([a, sample(30)])['classification'], 'unavailable')

    def test_nonlinear_inconsistent_series(self):
        self.assertEqual(tool.trend([sample(), sample(30, 30000000), sample(300)])
                         ['classification'], 'unavailable')

    def test_only_diag_is_sent_and_host_latency_separate(self):
        s = sample()
        s.update(lo=-2000, hi=1000000)
        with patch.object(tool, 'write_command') as write, \
             patch.object(tool, 'read_serial_reply', return_value=wire(s)), \
             patch.object(tool.secrets, 'token_hex', return_value='1234abcd'), \
             patch.object(tool.time, 'monotonic_ns', side_effect=[1000000000, 1020000000]), \
             patch.object(tool.time, 'time_ns', side_effect=[1792390000000000000, 1792390000020000000]):
            result = tool.query(object())
        self.assertEqual(write.call_args.args[1], '!RT1 1234abcd DIAG')
        self.assertEqual(result['read_duration_us'], 2000)
        self.assertEqual(result['round_trip_us'], 20000)
        self.assertEqual(result['rtc_minus_host_us'], [-20000, 1000000])
        self.assertEqual(result['uncertainty_width_us'], 1002000)

    def test_readonly_firmware_dispatch_and_monotonic_services(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'main/main.c').read_text()
        diagnostic = source.split('static void clock_diagnostic(')[1].split('static void time_command(')[0]
        for forbidden in ('settimeofday(', 'sync_wall_clock(', 'rtc_set_trusted(', 'esp_restart(', 'nvs_set_'):
            self.assertNotIn(forbidden, diagnostic)
        command = source.split('static void time_command(')[1].split('/* Fresh status')[0]
        self.assertLess(command.index('command.diagnostic'), command.index('command.set'))
        for filename in ('components/roundie_ota/ota.c', 'components/roundie_boot_guard/boot_guard.c'):
            logic = (root / filename).read_text()
            self.assertNotIn('gettimeofday(', logic)
            self.assertNotIn('time(NULL)', logic)


if __name__ == '__main__':
    unittest.main()
