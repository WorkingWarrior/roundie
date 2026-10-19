#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
int usb_serial_jtag_write_bytes(const void *data, size_t length, TickType_t ticks);
