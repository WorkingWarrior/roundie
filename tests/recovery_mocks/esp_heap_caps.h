#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *function_name);
size_t heap_caps_get_free_size(unsigned flags);
size_t heap_caps_get_minimum_free_size(unsigned flags);
size_t heap_caps_get_largest_free_block(unsigned flags);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
