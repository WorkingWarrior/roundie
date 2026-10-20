"""Production LCD IO startup block with mocked IDF; no electrical claims."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LcdClockTests(unittest.TestCase):
    def test_pinned_idf_divider(self):
        # Compile the actual selected S3 algorithm, not a reimplementation.
        path = Path('/home/solve/.espressif/v6.1/esp-idf/components/esp_hal_gpspi/esp32s3/include/hal/spi_ll.h')
        source = path.read_text()
        start = source.index('static inline int spi_ll_freq_for_pre_n(')
        end = source.index('static inline int spi_ll_master_set_clock(', start)
        block = source[start:end]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define SPI_LL_MAX_PRE_DIV_NUM 16
typedef union {
    uint32_t val;
    struct { uint32_t clkcnt_l:6, clkcnt_h:6, clkcnt_n:6, clkdiv_pre:4, reserved:9, clk_equ_sysclk:1; };
} spi_ll_clock_val_t;
static struct { spi_ll_clock_val_t clock; } GPSPI2;
'''
        harness += block + r'''
int main(void)
{
    spi_ll_clock_val_t r;
    assert(spi_ll_master_cal_clock(80000000, 20000000, 128, &r) == 20000000);
    assert(r.clkdiv_pre == 0 && r.clkcnt_n == 3 && r.clkcnt_h == 1);
    assert(spi_ll_master_cal_clock(80000000, 30000000, 128, &r) == 26666666);
    assert(r.clkdiv_pre == 0 && r.clkcnt_n == 2 && r.clkcnt_h == 0);
    assert(spi_ll_master_cal_clock(80000000, 40000000, 128, &r) == 40000000);
    assert(r.clkdiv_pre == 0 && r.clkcnt_n == 1 && r.clkcnt_h == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='roundie-divider-') as temporary:
            c = Path(temporary) / 'divider.c'
            exe = Path(temporary) / 'divider'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu23', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)],
                           check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_profiles_and_io_ownership(self):
        source = (ROOT / 'components/roundie_board/board.c').read_text()
        block = source[source.index('    const spi_bus_config_t bus_config = {',
                                     source.index('static esp_err_t start_lcd(')):
                       source.index('    size_t command_count = 0;')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lcd_config.h"
#define LCD_TRANSFER_BYTES ROUNDIE_LCD_TRANSFER_BYTES
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 99
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define SPICOMMON_BUSFLAG_QUAD 4
#define GPIO_NUM_40 40
#define GPIO_NUM_46 46
#define GPIO_NUM_45 45
#define GPIO_NUM_42 42
#define GPIO_NUM_41 41
#define GPIO_NUM_21 21
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef struct {
    int sclk_io_num, data0_io_num, data1_io_num, data2_io_num, data3_io_num;
    int data4_io_num, data5_io_num, data6_io_num, data7_io_num;
    int max_transfer_sz, flags;
} spi_bus_config_t;
typedef struct {
    int cs_gpio_num, dc_gpio_num, spi_mode, pclk_hz, trans_queue_depth;
    int lcd_cmd_bits, lcd_param_bits, cs_ena_pretrans, cs_ena_posttrans;
    struct { int quad_mode; } flags;
} esp_lcd_panel_io_spi_config_t;
typedef struct { void *lcd_io; bool spi_started; } roundie_board_t;
typedef struct { unsigned lcd_variant; } roundie_board_status_t;
static int step, fail_at, variant, devices;
static int spi_bus_initialize(int host, const spi_bus_config_t *c, int dma)
{
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO);
    assert(c->sclk_io_num == 40 && c->data0_io_num == 46 && c->data1_io_num == 45);
    assert(c->data2_io_num == 42 && c->data3_io_num == 41);
    assert(c->max_transfer_sz == 14464 && c->flags == SPICOMMON_BUSFLAG_QUAD);
    assert(step++ == 0);
    return fail_at == step ? -step : 0;
}
static int esp_lcd_new_panel_io_spi(int host, const esp_lcd_panel_io_spi_config_t *c, void **out)
{
    assert(host == SPI2_HOST && devices == 0);
    assert(c->cs_gpio_num == 21 && c->dc_gpio_num == -1);
    assert(c->spi_mode == 0 && c->flags.quad_mode == 1);
    assert(c->trans_queue_depth == 2 && c->lcd_cmd_bits == 32 && c->lcd_param_bits == 8);
    assert(step == 1 || step == 4);
    assert(c->pclk_hz == (step == 1 ? ROUNDIE_LCD_READ_HZ : ROUNDIE_LCD_WRITE_HZ));
    assert(c->cs_ena_pretrans == (step == 4) && c->cs_ena_posttrans == (step == 4));
    step++;
    if (fail_at == step) { return -step; }
    devices++;
    *out = (void *)(uintptr_t)step;
    return 0;
}
static int esp_lcd_panel_io_rx_param(void *io, int command, void *buffer, unsigned length)
{
    assert(io == (void *)2 && devices == 1 && step++ == 2);
    assert(command == ((0x0b << 24) | (0x04 << 8)) && length == 4);
    if (fail_at == step) { return -step; }
    memcpy(buffer, variant == 1 ? (uint8_t[]){0, 0x7f, 0x7f, 0x7f} :
                   variant == 2 ? (uint8_t[]){0, 2, 0x7f, 0x7f} : (uint8_t[4]){0}, 4);
    return 0;
}
static int esp_lcd_panel_io_del(void *io)
{
    assert(io == (void *)2 && devices == 1 && step++ == 3);
    if (fail_at == step) { return -step; }
    devices--;
    return 0;
}
static int start_io(roundie_board_t *board, roundie_board_status_t *status)
{
    int error;
'''
        harness += block + '\nreturn 0;\n}\n'
        harness += r'''
int main(void)
{
    assert(ROUNDIE_LCD_WIDTH * ROUNDIE_LCD_HEIGHT * 2 == 259200);
    assert(ROUNDIE_LCD_HEIGHT / ROUNDIE_LCD_STRIP_LINES == 18);
    /* 4-lane pixel-only microseconds; no rounding for these profiles. */
    assert(UINT64_C(259200) * 8 * 1000000 / 4 / 3000000 == 172800);
    for (variant = 0; variant <= 2; variant++) {
        for (fail_at = 0; fail_at <= 5; fail_at++) {
            step = devices = 0;
            roundie_board_t board = {0};
            roundie_board_status_t status = {0};
            int result = start_io(&board, &status);
            if (variant == 0 && (fail_at == 0 || fail_at > 3)) {
                assert(result == ESP_ERR_NOT_SUPPORTED && step == 3);
            } else if (fail_at != 0) {
                assert(result == -fail_at && step == fail_at);
            } else {
                assert(result == 0 && step == 5 && status.lcd_variant == (unsigned)variant);
                assert(devices == 1 && board.lcd_io == (void *)5);
            }
            if (step == 5 && result != 0) {
                assert(board.lcd_io == 0 && devices == 0);
            }
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='roundie-clock-') as temporary:
            directory = Path(temporary)
            (directory / 'sdkconfig.h').touch()
            c = directory / 'clock.c'
            c.write_text(harness)
            for hz in (3000000, 10000000, 20000000, 30000000, 40000000, 0, 60000000, 80000000):
                command = ['cc', '-std=c23', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', '-g',
                           f'-DCONFIG_ROUNDIE_LCD_WRITE_HZ={hz}', '-I' + temporary,
                           '-I' + str(ROOT / 'components/roundie_board'), str(c),
                           '-o', str(directory / 'clock')]
                compiled = subprocess.run(command, capture_output=True)
                if hz in (0, 60000000, 80000000):
                    self.assertNotEqual(compiled.returncode, 0)
                    self.assertIn(b'Unsupported 1.85B LCD write clock', compiled.stderr)
                else:
                    self.assertEqual(compiled.returncode, 0, compiled.stderr.decode())
                    subprocess.run([str(directory / 'clock')], check=True, timeout=5)
