#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_storage_session.h"

/* The USB side of storage mode (docs/usb-storage-mode-plan.md, gate G2): a FAT
 * volume in PSRAM behind an esp_blockdev, exposed as mass storage by
 * esp_tinyusb. TinyUSB callbacks only count and queue small events; everything
 * else is read from the GUI task. */

typedef struct {
    bool started;
    esp_err_t error;          /* first failure while starting, ESP_OK otherwise */
    const char *failed_step;
    bool host_owns;
    uint32_t io_calls;        /* host block reads and writes so far */
    uint32_t attached;
    uint32_t detached;
    uint32_t suspended;
    uint32_t resumed;
    uint32_t dropped_events;
} roundie_storage_usb_status_t;

esp_err_t roundie_storage_usb_start(void);
void roundie_storage_usb_status(roundie_storage_usb_status_t *out);
/* GUI task only. Next queued bus event, false when none. */
bool roundie_storage_usb_next_event(roundie_storage_bus_event_t *event);
/* GUI task only: give the volume back to the firmware. */
void roundie_storage_usb_take_back(void);
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
/* Development status port. Writes one line when a host has the port open. */
void roundie_storage_usb_report(const char *line);
/* True once the host sent the word EXITNOW to the port. */
bool roundie_storage_usb_exit_requested(void);
/* Answer to the confirmation dialog sent as the word INSTALL or SKIP; 0 none, 1 install, 2 skip. */
int roundie_storage_usb_take_dev_decision(void);
#endif
