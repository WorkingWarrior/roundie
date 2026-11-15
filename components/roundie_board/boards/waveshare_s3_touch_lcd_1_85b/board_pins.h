#pragma once

#include "driver/gpio.h"

/* Waveshare ESP32-S3-Touch-LCD-1.85B Rev1.1 wiring (docs/hardware-audit.md).
 * The only place in the firmware that names this board's GPIOs, buses and
 * I2C addresses. Do not reuse for the non-B 1.85: its wiring differs. */

/* Shared I2C master: touch, RTC, gauge and the audio codec. */
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_11
#define BOARD_I2C_SCL GPIO_NUM_10
#define BOARD_I2C_DEVICE_HZ 100000

/* CST816S. Rev1.1: INT is driven low on touch; it is an RTC GPIO. */
#define BOARD_TOUCH_ADDRESS 0x15
#define BOARD_TOUCH_RESET GPIO_NUM_1
#define BOARD_TOUCH_INT GPIO_NUM_4

/* PCF85063 RTC and BQ27220 fuel gauge. */
#define BOARD_RTC_ADDRESS 0x51
#define BOARD_GAUGE_ADDRESS 0x55

/* BOOT: active low, pulled high, the ROM Download strap; input only. */
#define BOARD_BOOT_BUTTON GPIO_NUM_0

/* ST77916 on QSPI. No TE line is documented for the 1.85B. */
#define BOARD_LCD_SPI_HOST SPI2_HOST
#define BOARD_LCD_SCLK GPIO_NUM_40
#define BOARD_LCD_DATA0 GPIO_NUM_46
#define BOARD_LCD_DATA1 GPIO_NUM_45
#define BOARD_LCD_DATA2 GPIO_NUM_42
#define BOARD_LCD_DATA3 GPIO_NUM_41
#define BOARD_LCD_CS GPIO_NUM_21
#define BOARD_LCD_RESET GPIO_NUM_3

/* Backlight PWM. */
#define BOARD_BACKLIGHT GPIO_NUM_5
#define BOARD_BACKLIGHT_TIMER LEDC_TIMER_1
#define BOARD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
