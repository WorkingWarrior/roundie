#pragma once
#include "ota_mock.h"
void mock_log(const char *tag, const char *format, ...);
const char *esp_err_to_name(esp_err_t error);
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
