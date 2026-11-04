#include "storage_usb.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_USB_STORAGE
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_blockdev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
#include "tinyusb_cdc_acm.h"
#endif

#define VOLUME_BYTES (1024U * 1024U)
#define SECTOR_BYTES 512U
#define MOUNT_PATH "/usbdisk"
#define EVENT_QUEUE 8U

static const char *TAG = "storage_usb";

static uint8_t *volume;
static esp_blockdev_t block_device;
static tinyusb_msc_storage_handle_t storage;
static roundie_storage_usb_status_t started_status;

static _Atomic uint32_t io_calls;
static _Atomic uint32_t attached_count;
static _Atomic uint32_t detached_count;
static _Atomic uint32_t suspended_count;
static _Atomic uint32_t resumed_count;
static _Atomic uint32_t dropped_count;
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
static _Atomic bool exit_word_seen;
static _Atomic int dev_decision; /* 0 none, 1 INSTALL, 2 SKIP */
#endif

/* Small bounded event queue: TinyUSB task produces, the GUI task consumes. */
static portMUX_TYPE queue_lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_storage_bus_event_t queue[EVENT_QUEUE];
static unsigned queue_head;
static unsigned queue_count;

static void queue_event(roundie_storage_bus_event_t event)
{
    portENTER_CRITICAL(&queue_lock);
    if (queue_count < EVENT_QUEUE) {
        queue[(queue_head + queue_count) % EVENT_QUEUE] = event;
        queue_count++;
    } else {
        atomic_fetch_add(&dropped_count, 1);
    }
    portEXIT_CRITICAL(&queue_lock);
}

bool roundie_storage_usb_next_event(roundie_storage_bus_event_t *event)
{
    bool found = false;
    portENTER_CRITICAL(&queue_lock);
    if (queue_count > 0) {
        *event = queue[queue_head];
        queue_head = (queue_head + 1) % EVENT_QUEUE;
        queue_count--;
        found = true;
    }
    portEXIT_CRITICAL(&queue_lock);
    return found;
}

/* Block device callbacks run in the TinyUSB task: memcpy on RAM only. */
static esp_err_t volume_read(esp_blockdev_handle_t device, uint8_t *destination, size_t destination_size,
                             uint64_t source, size_t length)
{
    (void)device;
    if (source + length > VOLUME_BYTES || length > destination_size) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(destination, volume + source, length);
    atomic_fetch_add(&io_calls, 1);
    return ESP_OK;
}

static esp_err_t volume_write(esp_blockdev_handle_t device, const uint8_t *source, uint64_t destination,
                              size_t length)
{
    (void)device;
    if (destination + length > VOLUME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(volume + destination, source, length);
    atomic_fetch_add(&io_calls, 1);
    return ESP_OK;
}

static esp_err_t volume_erase(esp_blockdev_handle_t device, uint64_t start, size_t length)
{
    (void)device;
    if (start + length > VOLUME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(volume + start, 0, length);
    return ESP_OK;
}

static esp_err_t volume_sync(esp_blockdev_handle_t device)
{
    (void)device;
    return ESP_OK;
}

static esp_err_t volume_ioctl(esp_blockdev_handle_t device, const uint8_t command, void *arguments)
{
    (void)device;
    (void)command;
    (void)arguments;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t volume_release(esp_blockdev_handle_t device)
{
    (void)device;
    return ESP_OK;
}

static const esp_blockdev_ops_t volume_ops = {
    .read = volume_read,
    .write = volume_write,
    .erase = volume_erase,
    .sync = volume_sync,
    .ioctl = volume_ioctl,
    .release = volume_release,
};

static void usb_event(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        atomic_fetch_add(&attached_count, 1);
        queue_event(ROUNDIE_STORAGE_BUS_ATTACHED);
        break;
    case TINYUSB_EVENT_DETACHED:
        atomic_fetch_add(&detached_count, 1);
        break;
    case TINYUSB_EVENT_SUSPENDED:
        atomic_fetch_add(&suspended_count, 1);
        queue_event(ROUNDIE_STORAGE_BUS_SUSPENDED);
        break;
    case TINYUSB_EVENT_RESUMED:
        atomic_fetch_add(&resumed_count, 1);
        queue_event(ROUNDIE_STORAGE_BUS_RESUMED);
        break;
    default:
        break;
    }
}

#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
static void cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buffer[32];
    size_t length = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buffer, sizeof(buffer), &length) != ESP_OK) {
        return;
    }
    /* Whole words, so stray bytes sent by a host cannot act. */
    static const char *const words[] = {"EXITNOW", "INSTALL", "SKIP"};
    static size_t matched[3];
    for (size_t index = 0; index < length; index++) {
        for (unsigned word = 0; word < 3; word++) {
            size_t size = strlen(words[word]);
            matched[word] = buffer[index] == (uint8_t)words[word][matched[word]] ? matched[word] + 1 :
                (buffer[index] == (uint8_t)words[word][0] ? 1 : 0);
            if (matched[word] == size) {
                matched[word] = 0;
                if (word == 0) {
                    atomic_store(&exit_word_seen, true);
                } else {
                    atomic_store(&dev_decision, (int)word);
                }
            }
        }
    }
}

void roundie_storage_usb_report(const char *line)
{
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, strlen(line));
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
}

bool roundie_storage_usb_exit_requested(void)
{
    return atomic_load(&exit_word_seen);
}

int roundie_storage_usb_take_dev_decision(void)
{
    return atomic_exchange(&dev_decision, 0);
}
#endif

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

static esp_err_t fail(const char *step, esp_err_t error)
{
    started_status.error = error;
    started_status.failed_step = step;
    ESP_LOGE(TAG, "%s: %s", step, esp_err_to_name(error));
    return error;
}

esp_err_t roundie_storage_usb_start(void)
{
    volume = heap_caps_calloc(1, VOLUME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (volume == NULL) {
        return fail("volume memory", ESP_ERR_NO_MEM);
    }
    block_device.device_flags.val = 0;
    block_device.geometry.disk_size = VOLUME_BYTES;
    block_device.geometry.read_size = SECTOR_BYTES;
    block_device.geometry.write_size = SECTOR_BYTES;
    block_device.geometry.erase_size = SECTOR_BYTES;
    block_device.ops = &volume_ops;

    const tinyusb_msc_driver_config_t driver_config = {0};
    esp_err_t error = tinyusb_msc_install_driver(&driver_config);
    if (error != ESP_OK) {
        return fail("msc driver", error);
    }
    const tinyusb_msc_storage_config_t storage_config = {
        .medium.blockdev = &block_device,
        .fat_fs = {
            .config = {.max_files = 4},
            .do_not_format = false,
            .format_flags = FM_ANY,
            .base_path = MOUNT_PATH,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    error = tinyusb_msc_new_storage_blockdev(&storage_config, &storage);
    if (error != ESP_OK) {
        return fail("msc storage", error);
    }
    /* The firmware owns the volume until the host connects. ASCII only: the
     * host decodes these names and texts with its own code page. */
    if (mkdir(MOUNT_PATH "/Inbox", 0777) != 0) {
        return fail("inbox folder", ESP_FAIL);
    }
    write_text(MOUNT_PATH "/README.TXT",
               "Roundie USB storage mode.\r\nCopy .rpk packages into the Inbox folder, then eject this drive.\r\n");
    const tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event, NULL);
    error = tinyusb_driver_install(&config);
    if (error != ESP_OK) {
        return fail("usb driver", error);
    }
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
    const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = cdc_rx};
    error = tinyusb_cdcacm_init(&acm);
    if (error != ESP_OK) {
        return fail("cdc status port", error);
    }
#endif
    started_status.started = true;
    ESP_LOGI(TAG, "USB storage started: %u KiB volume in PSRAM", VOLUME_BYTES / 1024U);
    return ESP_OK;
}

void roundie_storage_usb_status(roundie_storage_usb_status_t *out)
{
    *out = started_status;
    out->io_calls = atomic_load(&io_calls);
    out->attached = atomic_load(&attached_count);
    out->detached = atomic_load(&detached_count);
    out->suspended = atomic_load(&suspended_count);
    out->resumed = atomic_load(&resumed_count);
    out->dropped_events = atomic_load(&dropped_count);
    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    out->host_owns = storage != NULL && tinyusb_msc_get_storage_mount_point(storage, &mount_point) == ESP_OK &&
        mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

void roundie_storage_usb_take_back(void)
{
    if (storage != NULL) {
        esp_err_t error = tinyusb_msc_set_storage_mount_point(storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Take back: %s", esp_err_to_name(error));
        }
    }
}
#else
esp_err_t roundie_storage_usb_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void roundie_storage_usb_status(roundie_storage_usb_status_t *out)
{
    *out = (roundie_storage_usb_status_t){.error = ESP_ERR_NOT_SUPPORTED, .failed_step = "not built"};
}

bool roundie_storage_usb_next_event(roundie_storage_bus_event_t *event)
{
    (void)event;
    return false;
}

void roundie_storage_usb_take_back(void)
{
}
#endif
