#pragma once
#include "ota_mock.h"
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 100
#define ESP_ERR_NVS_INVALID_LENGTH 101
#define ESP_ERR_NVS_TYPE_MISMATCH 102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 103
esp_err_t nvs_open(const char *name, unsigned mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
typedef struct {
    size_t used_entries;
    size_t free_entries;
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
} nvs_stats_t;
esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *nvs_stats);
