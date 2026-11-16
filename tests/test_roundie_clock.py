"""Read-only diagnostic protocol and interval-rate regression tests."""
from pathlib import Path
import os
import re
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
                    ('retention', ['components/roundie_time/clock_retention.c', 'tests/test_clock_retention.c']),
                    ('codec', ['components/roundie_board/rtc_time.c', 'components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b/sensor_codec.c',
                               'tests/test_sensor_codec.c']),
                    ('protocol', ['components/roundie_time/time_protocol.c', 'components/roundie_board/rtc_time.c',
                                  'tests/test_time_protocol.c'])):
                    output = str(Path(folder) / f'{name}-{standard}')
                    subprocess.run(['cc', f'-std={standard}', '-Wall', '-Wextra', '-Werror',
                                    '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                    '-Imain', '-Icomponents/roundie_time/include',
                                    '-Icomponents/roundie_board/boards/waveshare_s3_touch_lcd_1_85b',
                                    '-Icomponents/roundie_board/include', *sources, '-o', output],
                                   cwd=root, check=True, capture_output=True)
                    subprocess.run([output], check=True, capture_output=True)

    def test_shared_time_code_on_emulated_hardware(self):
        """M3: components/roundie_time with the desktop's timekeeping model and
        a virtual clock (tests/test_wall_clock.c)."""
        root = Path(__file__).resolve().parents[1]
        sources = ['tests/test_wall_clock.c', 'platform/linux/clock.c',
                   *(f'components/roundie_time/{name}.c'
                     for name in ('wall_clock', 'local_time', 'clock_retention', 'time_protocol')),
                   'components/roundie_board/rtc_time.c',
                   'components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b/sensor_codec.c']
        includes = ['tests/time_mocks', 'platform/linux/include', 'platform/linux', 'components/roundie_time/include',
                    'components/roundie_board/include', 'components/roundie_board/boards/waveshare_s3_touch_lcd_1_85b',
                    'components/roundie_elf/include', 'components/roundie_platform/include']
        with tempfile.TemporaryDirectory() as folder:
            for standard in ('gnu11', 'gnu23'):
                output = str(Path(folder) / f'wall-clock-{standard}')
                subprocess.run(['cc', f'-std={standard}', '-Wall', '-Wextra', '-Werror', '-g',
                                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                *(f'-I{path}' for path in includes), *sources, '-o', output],
                               cwd=root, check=True, capture_output=True)
                result = subprocess.run([output, folder], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for part in ('without RTC', 'with RTC', 'calendar and zones', 'extremes'):
                    self.assertIn(f'{part}: OK', result.stdout)

    def test_reset_reasons_that_keep_time(self):
        """The firmware port's reset table (idf/clock_port.c) against every
        ESP-IDF reset reason, and the desktop model's resets against it."""
        root = Path(__file__).resolve().parents[1]
        idf = os.environ.get('IDF_PATH')
        if not idf:
            self.skipTest('IDF_PATH not set')
        header = (Path(idf) / 'components/esp_system/include/esp_system.h').read_text()
        body = header[header.index('typedef enum {', header.index('ESP_RST_UNKNOWN') - 200):
                      header.index('} esp_reset_reason_t;')]
        reasons = re.findall(r'\b(ESP_RST_[A-Z_]+)\b', body)
        self.assertIn('ESP_RST_POWERON', reasons)
        port = (root / 'components/roundie_time/idf/clock_port.c').read_text()
        start = port.index('bool roundie_clock_port_reset_kept_domain(')
        logic = port[start:port.index('\n}\n', start) + 3]
        keeps = {'ESP_RST_SW', 'ESP_RST_PANIC', 'ESP_RST_INT_WDT', 'ESP_RST_TASK_WDT', 'ESP_RST_WDT',
                 'ESP_RST_DEEPSLEEP', 'ESP_RST_USB', 'ESP_RST_JTAG', 'ESP_RST_CPU_LOCKUP'}
        table = ',\n'.join(f'    {{{r}, {int(r in keeps)}, "{r}"}}' for r in reasons)
        harness = r"""
#include <stdbool.h>
#include <stdio.h>
typedef enum {""" + ', '.join(reasons) + r"""} esp_reset_reason_t;
static esp_reset_reason_t reason;
static esp_reset_reason_t esp_reset_reason(void) { return reason; }
""" + logic + r"""
static const struct { esp_reset_reason_t reason; int keeps; const char *name; } reasons[] = {
""" + table + r"""
};
int main(void)
{
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        reason = reasons[i].reason;
        int code = -1;
        bool kept = roundie_clock_port_reset_kept_domain(&code);
        if (kept != (reasons[i].keeps != 0) || code != (int)reason) {
            printf("unexpected for %s\n", reasons[i].name);
            return 1;
        }
    }
    puts("reset reasons: OK");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as folder:
            c = Path(folder) / 'reset.c'
            c.write_text(harness)
            output = str(Path(folder) / 'reset')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                            '-fsanitize=address,undefined', str(c), '-o', output],
                           cwd=root, check=True, capture_output=True)
            subprocess.run([output], check=True, capture_output=True)
        # The desktop's resets map to ESP-IDF reasons and keep time alike.
        desktop = (root / 'platform/linux/clock.c').read_text()
        kept = desktop[desktop.index('bool roundie_clock_port_reset_kept_domain('):]
        kept = kept[:kept.index('\n}\n')]
        model = {'SW': 'ESP_RST_SW', 'PANIC': 'ESP_RST_PANIC', 'TASK_WDT': 'ESP_RST_TASK_WDT',
                 'POWER': 'ESP_RST_POWERON', 'BROWNOUT': 'ESP_RST_BROWNOUT', 'PIN': 'ESP_RST_EXT'}
        for name, reason in model.items():
            self.assertIn(reason, reasons)
            self.assertEqual(f'ROUNDIE_DESKTOP_RESET_{name}' in kept, reason in keeps, name)

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
        command = source.split('static void time_command(')[1].split('shared->rtc = rtc')[0]
        self.assertLess(command.index('command.diagnostic'), command.index('command.set'))
        # Monotonic services, and the shared time code, which reaches the
        # system clock only through roundie_clock_port.h.
        for filename in ('components/roundie_ota/ota.c', 'components/roundie_boot_guard/boot_guard.c',
                         'components/roundie_power/power.c', 'main/power_gui.c', 'main/power_manager.c',
                         'components/roundie_time/wall_clock.c', 'components/roundie_time/local_time.c'):
            logic = (root / filename).read_text()
            self.assertNotIn('gettimeofday(', logic)
            self.assertNotIn('time(NULL)', logic)


if __name__ == '__main__':
    unittest.main()
