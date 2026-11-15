#pragma once

/* Desktop: the ESP-IDF log macros that Core code uses, written to stderr as
 * "I power: message". No log levels or timestamps. */

#include <stdio.h>

#define ROUNDIE_DESKTOP_LOG(level, tag, format, ...) \
    fprintf(stderr, level " %s: " format "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGE(tag, format, ...) ROUNDIE_DESKTOP_LOG("E", tag, format __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ROUNDIE_DESKTOP_LOG("W", tag, format __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ROUNDIE_DESKTOP_LOG("I", tag, format __VA_OPT__(, ) __VA_ARGS__)
