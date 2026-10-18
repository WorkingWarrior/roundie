#pragma once
typedef struct { unsigned address; unsigned size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
