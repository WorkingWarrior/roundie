#pragma once
#include "ota_mock.h"
#include "freertos/FreeRTOS.h"
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks);
int usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t ticks);
