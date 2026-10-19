#include "roundie_board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_init_tables.h"
#include "sensor_codec.h"

#define LCD_WIDTH 360
#define LCD_HEIGHT 360
#define LCD_STRIP_LINES 20
#define LCD_TRANSFER_BYTES (LCD_WIDTH * LCD_STRIP_LINES * 2)
#define I2C_TIMEOUT_MS 100

static const char *TAG = "roundie_board";

static void log_cleanup_error(const char *resource, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Cleanup %s: %s", resource, esp_err_to_name(error));
    }
}

struct roundie_board {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t rtc;
    i2c_master_dev_handle_t gauge;
    esp_lcd_panel_io_handle_t touch_io;
    esp_lcd_touch_handle_t touch;
    volatile bool touch_irq_pending;
    bool touch_polling;
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t panel;
    bool lcd_reset_owned;
    bool spi_started;
    bool backlight_started;
};

static esp_err_t read_register(i2c_master_dev_handle_t device, uint8_t address,
                               uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(device, &address, 1, data, length,
                                       I2C_TIMEOUT_MS);
}

static esp_err_t start_i2c(roundie_board_t *board, roundie_board_status_t *status)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_11,
        .scl_io_num = GPIO_NUM_10,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t error = i2c_new_master_bus(&bus_config, &board->i2c_bus);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "I2C master start: %s", esp_err_to_name(error));
        status->touch = error;
        status->rtc = error;
        status->gauge = error;
        return error;
    }

    status->touch = i2c_master_probe(board->i2c_bus, 0x15, I2C_TIMEOUT_MS);
    status->rtc = i2c_master_probe(board->i2c_bus, 0x51, I2C_TIMEOUT_MS);
    status->gauge = i2c_master_probe(board->i2c_bus, 0x55, I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "I2C probe CST816S 0x15: %s", esp_err_to_name(status->touch));
    ESP_LOGI(TAG, "I2C probe PCF85063 0x51: %s", esp_err_to_name(status->rtc));
    ESP_LOGI(TAG, "I2C probe BQ27220 0x55: %s", esp_err_to_name(status->gauge));

    const i2c_device_config_t rtc_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x51,
        .scl_speed_hz = 100000,
    };
    esp_err_t rtc_add = i2c_master_bus_add_device(board->i2c_bus, &rtc_config,
                                                   &board->rtc);
    if (rtc_add != ESP_OK) {
        ESP_LOGE(TAG, "RTC handle: %s", esp_err_to_name(rtc_add));
        status->rtc = rtc_add;
    }

    const i2c_device_config_t gauge_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x55,
        .scl_speed_hz = 100000,
    };
    esp_err_t gauge_add = i2c_master_bus_add_device(board->i2c_bus,
                                                     &gauge_config, &board->gauge);
    if (gauge_add != ESP_OK) {
        ESP_LOGE(TAG, "Gauge handle: %s", esp_err_to_name(gauge_add));
        status->gauge = gauge_add;
    }
    return ESP_OK;
}

static void touch_interrupt(esp_lcd_touch_handle_t touch)
{
    roundie_board_t *board = touch->config.user_data;
    board->touch_irq_pending = true;
}

static esp_err_t start_touch(roundie_board_t *board)
{
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_config.scl_speed_hz = 100000;
    esp_err_t error = esp_lcd_new_panel_io_i2c(board->i2c_bus, &io_config,
                                               &board->touch_io);
    if (error != ESP_OK) {
        return error;
    }

    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_1,
        .int_gpio_num = GPIO_NUM_4,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .interrupt_callback = NULL,
        .user_data = board,
    };
    error = esp_lcd_touch_new_i2c_cst816s(board->touch_io, &touch_config,
                                          &board->touch);
    if (error != ESP_OK) {
        goto cleanup;
    }
    error = esp_lcd_touch_register_interrupt_callback(board->touch,
                                                       touch_interrupt);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "CST816S IRQ registration: %s", esp_err_to_name(error));
        goto cleanup;
    }
    return ESP_OK;

cleanup:
    if (board->touch != NULL) {
        log_cleanup_error("touch after failed start",
                          esp_lcd_touch_del(board->touch));
        board->touch = NULL;
    }
    log_cleanup_error("touch IO after failed start",
                      esp_lcd_panel_io_del(board->touch_io));
    board->touch_io = NULL;
    return error;
}

static esp_err_t start_lcd(roundie_board_t *board, roundie_board_status_t *status)
{
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_3,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t error = gpio_config(&reset_config);
    if (error != ESP_OK) {
        return error;
    }
    board->lcd_reset_owned = true;
    error = gpio_set_level(GPIO_NUM_3, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    error = gpio_set_level(GPIO_NUM_3, 1);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    const spi_bus_config_t bus_config = {
        .sclk_io_num = GPIO_NUM_40,
        .data0_io_num = GPIO_NUM_46,
        .data1_io_num = GPIO_NUM_45,
        .data2_io_num = GPIO_NUM_42,
        .data3_io_num = GPIO_NUM_41,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_TRANSFER_BYTES + 64,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    error = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        return error;
    }
    board->spi_started = true;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = GPIO_NUM_21,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = 1,
    };
    error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                     &io_config, &board->lcd_io);
    if (error != ESP_OK) {
        return error;
    }

    uint8_t panel_id[4] = {0};
    const int read_id_command = (0x0b << 24) | (0x04 << 8);
    error = esp_lcd_panel_io_rx_param(board->lcd_io, read_id_command,
                                      panel_id, sizeof(panel_id));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD ID read failed: %s", esp_err_to_name(error));
        return error;
    }
    ESP_LOGI(TAG, "LCD ID: %02x %02x %02x %02x", panel_id[0], panel_id[1],
             panel_id[2], panel_id[3]);
    if (memcmp(panel_id, (const uint8_t[]){0x00, 0x7f, 0x7f, 0x7f}, 4) == 0) {
        status->lcd_variant = 1;
    } else if (memcmp(panel_id, (const uint8_t[]){0x00, 0x02, 0x7f, 0x7f}, 4) == 0) {
        status->lcd_variant = 2;
    } else {
        ESP_LOGE(TAG, "Unsupported LCD ID; no initialization table selected");
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t command_count = 0;
    const st77916_lcd_init_cmd_t *commands =
        roundie_lcd_table(status->lcd_variant, &command_count);
    st77916_vendor_config_t vendor_config = {
        .init_cmds = commands,
        .init_cmds_size = command_count,
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_3,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    /* The early ID read needed reset GPIO before a panel existed. Transfer
     * ownership to the panel driver before it configures the same pin. */
    error = gpio_reset_pin(GPIO_NUM_3);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_new_panel_st77916(board->lcd_io, &panel_config, &board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_reset(board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_init(board->panel);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_lcd_panel_disp_on_off(board->panel, true);
    if (error != ESP_OK) {
        return error;
    }
    ESP_LOGI(TAG, "ST77916 variant %u initialized at 3 MHz QSPI",
             status->lcd_variant);

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    error = ledc_timer_config(&timer_config);
    if (error != ESP_OK) {
        return error;
    }
    const ledc_channel_config_t channel_config = {
        .gpio_num = GPIO_NUM_5,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    error = ledc_channel_config(&channel_config);
    if (error != ESP_OK) {
        return error;
    }
    board->backlight_started = true;
    return ESP_OK;
}

static esp_err_t start_board(roundie_board_t **out_board,
                              roundie_board_status_t *status, bool recovery)
{
    if (out_board == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_board = NULL;
    memset(status, 0, sizeof(*status));
    status->touch = ESP_ERR_NOT_FOUND;
    status->rtc = ESP_ERR_NOT_FOUND;
    status->gauge = ESP_ERR_NOT_FOUND;

    roundie_board_t *board = calloc(1, sizeof(*board));
    if (board == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!recovery) {
        esp_err_t i2c_error = start_i2c(board, status);
        if (i2c_error != ESP_OK) {
            roundie_board_stop(board);
            return i2c_error;
        }
        /* CST816S can remain in reset until the driver toggles GPIO1. */
        status->touch = start_touch(board);
        ESP_LOGI(TAG, "CST816S driver: %s", esp_err_to_name(status->touch));
    }

    esp_err_t error = start_lcd(board, status);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD start failed: %s", esp_err_to_name(error));
        roundie_board_stop(board);
        return error;
    }
    *out_board = board;
    return ESP_OK;
}

esp_err_t roundie_board_start(roundie_board_t **out_board,
                              roundie_board_status_t *status)
{
    return start_board(out_board, status, false);
}

esp_err_t roundie_board_start_recovery(roundie_board_t **out_board,
                                      roundie_board_status_t *status)
{
    return start_board(out_board, status, true);
}

void roundie_board_stop(roundie_board_t *board)
{
    if (board == NULL) {
        return;
    }
    if (board->backlight_started) {
        log_cleanup_error("backlight PWM",
                          ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
        log_cleanup_error("backlight GPIO", gpio_reset_pin(GPIO_NUM_5));
    }
    if (board->panel != NULL) {
        log_cleanup_error("LCD panel", esp_lcd_panel_del(board->panel));
    }
    if (board->lcd_io != NULL) {
        log_cleanup_error("LCD IO", esp_lcd_panel_io_del(board->lcd_io));
    }
    if (board->spi_started) {
        log_cleanup_error("LCD SPI bus", spi_bus_free(SPI2_HOST));
    }
    if (board->lcd_reset_owned) {
        log_cleanup_error("LCD reset GPIO", gpio_reset_pin(GPIO_NUM_3));
    }
    if (board->touch != NULL) {
        log_cleanup_error("touch", esp_lcd_touch_del(board->touch));
    }
    if (board->touch_io != NULL) {
        log_cleanup_error("touch IO", esp_lcd_panel_io_del(board->touch_io));
    }
    if (board->gauge != NULL) {
        log_cleanup_error("gauge I2C device",
                          i2c_master_bus_rm_device(board->gauge));
    }
    if (board->rtc != NULL) {
        log_cleanup_error("RTC I2C device",
                          i2c_master_bus_rm_device(board->rtc));
    }
    if (board->i2c_bus != NULL) {
        log_cleanup_error("I2C bus", i2c_del_master_bus(board->i2c_bus));
    }
    free(board);
}

esp_lcd_panel_handle_t roundie_board_panel(roundie_board_t *board)
{
    return board->panel;
}

esp_lcd_panel_io_handle_t roundie_board_panel_io(roundie_board_t *board)
{
    return board->lcd_io;
}

esp_err_t roundie_board_touch_read(roundie_board_t *board, bool *pressed,
                                   uint16_t *x, uint16_t *y)
{
    if (board == NULL || pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (board->touch == NULL) {
        *pressed = false;
        return ESP_ERR_NOT_FOUND;
    }
    if (!board->touch_irq_pending && !board->touch_polling) {
        *pressed = false;
        return ESP_OK;
    }
    board->touch_irq_pending = false;
    esp_err_t error = esp_lcd_touch_read_data(board->touch);
    if (error != ESP_OK) {
        board->touch_polling = false;
        *pressed = false;
        return error;
    }
    uint8_t point_count = 0;
    esp_lcd_touch_point_data_t point = {0};
    error = esp_lcd_touch_get_data(board->touch, &point, &point_count, 1);
    if (error != ESP_OK) {
        board->touch_polling = false;
        *pressed = false;
        return error;
    }
    *pressed = point_count > 0;
    board->touch_polling = *pressed;
    if (*pressed) {
        *x = point.x;
        *y = point.y;
    }
    return ESP_OK;
}

esp_err_t roundie_board_backlight_set(roundie_board_t *board, uint8_t percent)
{
    if (board == NULL || !board->backlight_started || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t duty = 1023U * percent / 100U;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error != ESP_OK) {
        return error;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static roundie_sensor_state_t sensor_error(roundie_board_t *board,
                                            uint8_t address, esp_err_t error)
{
    if (error == ESP_ERR_NOT_FOUND) {
        return ROUNDIE_SENSOR_NOT_FOUND;
    }
    if (board->i2c_bus != NULL &&
        i2c_master_probe(board->i2c_bus, address, I2C_TIMEOUT_MS) ==
            ESP_ERR_NOT_FOUND) {
        return ROUNDIE_SENSOR_NOT_FOUND;
    }
    return ROUNDIE_SENSOR_COMM_ERROR;
}

void roundie_board_rtc_read(roundie_board_t *board, roundie_rtc_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    memset(sample, 0, sizeof(*sample));
    sample->state = ROUNDIE_SENSOR_NOT_FOUND;
    sample->error = ESP_ERR_NOT_FOUND;
    sample->observed_us = esp_timer_get_time();
    if (board == NULL || board->rtc == NULL) {
        return;
    }
    uint8_t control;
    esp_err_t error = read_register(board->rtc, 0x00, &control, 1);
    if (error == ESP_OK) {
        uint8_t raw[7];
        error = read_register(board->rtc, 0x04, raw, sizeof(raw));
        if (error == ESP_OK) {
            sample->observed_us = esp_timer_get_time();
            bool calendar_valid = roundie_rtc_decode(raw, &sample->time,
                                                     &sample->oscillator_stopped);
            if ((control & 0xa2) != 0 || !calendar_valid ||
                sample->oscillator_stopped) {
                sample->state = ROUNDIE_SENSOR_INVALID;
                sample->error = ESP_ERR_INVALID_RESPONSE;
            } else {
                sample->state = ROUNDIE_SENSOR_VALID;
                sample->error = ESP_OK;
            }
            return;
        }
    }
    sample->error = error;
    sample->state = sensor_error(board, 0x51, error);
}

esp_err_t roundie_board_rtc_set_trusted(roundie_board_t *board,
                                         const roundie_rtc_time_t *time)
{
    uint8_t raw[8];
    if (board == NULL || time == NULL || !roundie_rtc_encode(time, &raw[1])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (board->rtc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t control;
    esp_err_t error = read_register(board->rtc, 0x00, &control, 1);
    if (error != ESP_OK) {
        return error;
    }
    if ((control & 0xa2) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    raw[0] = 0x04;
    /* NXP Rev. 7.3 §§7.3.1.1, 7.4: write seconds..years in one access.
     * Encoded seconds writes OS=0. No control/alarm/offset bits are changed. */
    error = i2c_master_transmit(board->rtc, raw, sizeof(raw), I2C_TIMEOUT_MS);
    if (error != ESP_OK) {
        return error;
    }
    roundie_rtc_sample_t verified;
    roundie_board_rtc_read(board, &verified);
    if (verified.state != ROUNDIE_SENSOR_VALID) {
        return verified.error;
    }
    int64_t expected_seconds;
    int64_t actual_seconds;
    if (!roundie_rtc_unix_seconds(time, &expected_seconds) ||
        !roundie_rtc_unix_seconds(&verified.time, &actual_seconds) ||
        !roundie_rtc_readback_matches(expected_seconds, actual_seconds)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t gauge_read_word(roundie_board_t *board, uint8_t command,
                                 uint16_t *value)
{
    uint8_t raw[2];
    esp_err_t error = read_register(board->gauge, command, raw, sizeof(raw));
    if (error == ESP_OK) {
        *value = roundie_gauge_word(raw);
    }
    return error;
}

static void sample_error(roundie_board_t *board, roundie_battery_sample_t *sample,
                         esp_err_t error)
{
    if (error != ESP_OK && sample->error == ESP_OK) {
        sample->error = error;
        sample->state = sensor_error(board, 0x55, error);
    }
}

void roundie_board_battery_read(roundie_board_t *board,
                                 roundie_battery_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    memset(sample, 0, sizeof(*sample));
    sample->state = ROUNDIE_SENSOR_NOT_FOUND;
    sample->error = ESP_ERR_NOT_FOUND;
    sample->observed_us = esp_timer_get_time();
    if (board == NULL || board->gauge == NULL) {
        return;
    }
    esp_err_t error = gauge_read_word(board, 0x0a, &sample->battery_flags);
    if (error != ESP_OK) {
        sample->error = error;
        sample->state = sensor_error(board, 0x55, error);
        return;
    }
    sample->battery_present = (sample->battery_flags & 0x0008) != 0;
    if (!sample->battery_present) {
        sample->state = ROUNDIE_SENSOR_BATTERY_ABSENT;
        sample->error = ESP_OK;
        return;
    }
    error = gauge_read_word(board, 0x3a, &sample->operation_flags);
    if (error != ESP_OK) {
        sample->error = error;
        sample->state = sensor_error(board, 0x55, error);
        return;
    }
    sample->initialized = (sample->operation_flags & 0x0020) != 0 &&
                          (sample->operation_flags & 0x0400) == 0;
    sample->discharging = (sample->battery_flags & 0x0001) != 0;
    sample->sleeping = (sample->battery_flags & 0x1000) != 0;
    sample->state = sample->initialized ? ROUNDIE_SENSOR_VALID :
                                        ROUNDIE_SENSOR_UNCALIBRATED;
    sample->error = ESP_OK;

    uint16_t value;
    error = gauge_read_word(board, 0x08, &value);
    if (error == ESP_OK) {
        sample->voltage_mv = value;
        sample->voltage_valid = value > 0 && value <= 6000;
        if (!sample->voltage_valid) {
            sample->state = ROUNDIE_SENSOR_INVALID;
            sample->error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    sample_error(board, sample, error);
    error = gauge_read_word(board, 0x12, &value);
    if (error == ESP_OK) {
        sample->full_mah = value;
        sample->full_valid = value > 0 && value != UINT16_MAX;
    }
    sample_error(board, sample, error);
    error = gauge_read_word(board, 0x10, &value);
    if (error == ESP_OK) {
        sample->remaining_mah = value;
        sample->remaining_valid = sample->full_valid && value <= sample->full_mah;
    }
    sample_error(board, sample, error);
    error = gauge_read_word(board, 0x2c, &value);
    if (error == ESP_OK) {
        sample->soc_percent = value;
        sample->soc_valid = sample->initialized && sample->full_valid &&
                            sample->remaining_valid && value <= 100;
    }
    sample_error(board, sample, error);
    error = gauge_read_word(board, 0x0c, &value);
    if (error == ESP_OK) {
        sample->current_ma = roundie_gauge_current(value);
        sample->current_valid = true;
    }
    sample_error(board, sample, error);
    error = gauge_read_word(board, 0x06, &value);
    if (error == ESP_OK) {
        sample->temperature_deci_c = roundie_gauge_deci_c(value);
        sample->temperature_valid = value >= 1732 && value <= 3732;
    }
    sample_error(board, sample, error);
    if (sample->initialized && (!sample->full_valid || !sample->remaining_valid ||
                                !sample->soc_valid) && sample->error == ESP_OK) {
        sample->state = ROUNDIE_SENSOR_UNCALIBRATED;
    }
    sample->observed_us = esp_timer_get_time();
}
