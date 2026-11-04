#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

/* Lab probe for the USB-OTG recovery question (docs/usb-architecture.md).
 * It starts TinyUSB with a CDC port on the internal PHY and reports bus
 * events. It never calls esp_ota_mark_app_valid_cancel_rollback(), so the
 * next reset returns to the previously confirmed firmware. */

#define REPORT_PERIOD_MS 1000
#define SELF_RESET_AFTER_S 1500 /* the probe resets itself, which rolls it back */

static const char *TAG = "otg_probe";
static volatile uint32_t attached_events;
static volatile uint32_t detached_events;
static volatile uint32_t suspended_events;
static volatile uint32_t resumed_events;
static volatile uint32_t rx_bytes;
static volatile bool reset_requested;

static void usb_event(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        attached_events++;
        break;
    case TINYUSB_EVENT_DETACHED:
        detached_events++;
        break;
    case TINYUSB_EVENT_SUSPENDED:
        suspended_events++;
        break;
    case TINYUSB_EVENT_RESUMED:
        resumed_events++;
        break;
    default:
        break;
    }
}

static void cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buffer[32];
    size_t length = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buffer, sizeof(buffer), &length) != ESP_OK) {
        return;
    }
    rx_bytes += (uint32_t)length;
    /* A reset needs the whole word, so stray bytes cannot trigger it. */
    static const char word[] = "RESETNOW";
    static size_t matched;
    for (size_t index = 0; index < length; index++) {
        matched = buffer[index] == (uint8_t)word[matched] ? matched + 1 : (buffer[index] == 'R' ? 1 : 0);
        if (matched == sizeof(word) - 1) {
            reset_requested = true;
            matched = 0;
        }
    }
}

void app_main(void)
{
    const tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event, NULL);
    esp_err_t error = tinyusb_driver_install(&config);
    ESP_LOGI(TAG, "driver install: %s", esp_err_to_name(error));
    const tinyusb_config_cdcacm_t acm = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx,
    };
    error = tinyusb_cdcacm_init(&acm);
    ESP_LOGI(TAG, "cdc init: %s", esp_err_to_name(error));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        char line[160];
        int length = snprintf(line, sizeof(line),
            "OTGPROBE up=%" PRIu32 " attached=%" PRIu32 " detached=%" PRIu32
            " suspended=%" PRIu32 " resumed=%" PRIu32 " rx=%" PRIu32 " reset_reason=%d\r\n",
            uptime_s, attached_events, detached_events, suspended_events, resumed_events, rx_bytes,
            (int)esp_reset_reason());
        if (length > 0 && tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, (size_t)length);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
        if (reset_requested || uptime_s >= SELF_RESET_AFTER_S) {
            ESP_LOGW(TAG, "software reset");
            esp_restart();
        }
    }
}
