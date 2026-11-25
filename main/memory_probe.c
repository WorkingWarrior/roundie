#include "memory_probe.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

/* Upper bounds keep the probe from starving the system: internal free memory
 * is about 150 KB with a 77 KB largest block on the 1.85B (E0 baseline). */
#define PROBE_INTERNAL_MAX (32U * 1024U)
#define PROBE_PSRAM_MAX (1024U * 1024U)

/* Only the system worker calls this; no lock is needed. */
static void *held_block;
static uint32_t held_bytes;
static uint32_t held_caps;

static const char *caps_name(uint32_t caps)
{
    if (caps == MALLOC_CAP_SPIRAM) {
        return "PSRAM";
    }
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        return "INTERNAL";
    }
    return "NONE";
}

/* "INTERNAL <bytes>" or "PSRAM <bytes>", decimal, within the bounds. */
static bool parse_alloc(const char *text, uint32_t *caps, uint32_t *bytes)
{
    uint32_t limit = 0;
    if (strncmp(text, "INTERNAL ", 9) == 0) {
        *caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        limit = PROBE_INTERNAL_MAX;
        text += 9;
    } else if (strncmp(text, "PSRAM ", 6) == 0) {
        *caps = MALLOC_CAP_SPIRAM;
        limit = PROBE_PSRAM_MAX;
        text += 6;
    } else {
        return false;
    }
    if (*text == '\0' || strlen(text) > 7) {
        return false;
    }
    uint32_t value = 0;
    for (const char *digit = text; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        value = value * 10U + (uint32_t)(*digit - '0');
    }
    if (value == 0 || value > limit) {
        return false;
    }
    *bytes = value;
    return true;
}

bool roundie_memory_probe_command(const char *operation, const char *id, int parsed)
{
    if (strcmp(operation, "MEMPROBE") != 0 && strncmp(operation, "MEMPROBE ", 9) != 0) {
        return false;
    }
    esp_err_t error = parsed;
    const char *kind = "STATUS";
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (error == ESP_OK && strncmp(operation, "MEMPROBE ALLOC ", 15) == 0) {
        kind = "ALLOC";
        uint32_t caps = 0;
        uint32_t bytes = 0;
        if (!parse_alloc(operation + 15, &caps, &bytes)) {
            error = ESP_ERR_INVALID_ARG;
        } else if (held_block != NULL) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            held_block = heap_caps_malloc(bytes, caps);
            if (held_block == NULL) {
                error = ESP_ERR_NO_MEM;
            } else {
                /* Touch every byte so the block is really in use. */
                memset(held_block, 0xA5, bytes);
                held_bytes = bytes;
                held_caps = caps;
            }
        }
    } else if (error == ESP_OK && strcmp(operation, "MEMPROBE FREE") == 0) {
        kind = "FREE";
        if (held_block == NULL) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            heap_caps_free(held_block);
            held_block = NULL;
            held_bytes = 0;
            held_caps = 0;
        }
    } else if (error == ESP_OK && strcmp(operation, "MEMPROBE") != 0) {
        error = ESP_ERR_INVALID_ARG;
    }
    char response[256];
    int length = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"memprobe\":\"%s\",\"held\":%" PRIu32 ",\"caps\":\"%s\","
        "\"internal_before\":%zu,\"internal_after\":%zu,\"psram_before\":%zu,\"psram_after\":%zu}\n",
        id, (int)error, kind, held_bytes, caps_name(held_caps), internal_before,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), psram_before,
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}
#else
bool roundie_memory_probe_command(const char *operation, const char *id, int parsed)
{
    (void)operation;
    (void)id;
    (void)parsed;
    return false;
}
#endif
