#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_blockdev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

/* Lab probe for the USB mass-storage question (docs/usb-architecture.md): a
 * FAT volume in PSRAM that the host sees as a drive with folders. It reports
 * ownership changes and the files left in Inbox. It never marks itself valid,
 * so the next reset returns to the previously confirmed firmware. */

#define RAMDISK_BYTES (4U * 1024U * 1024U)
#define SECTOR_BYTES 512U
#define MOUNT_PATH "/ramdisk"
#define REPORT_PERIOD_MS 1000
#define SELF_RESET_AFTER_S 2400

static const char *TAG = "msc_probe";

static uint8_t *ramdisk;
static volatile uint32_t read_calls;
static volatile uint32_t write_calls;
static volatile uint64_t read_bytes;
static volatile uint64_t write_bytes;
static volatile uint32_t attached_events;
static volatile uint32_t detached_events;
static volatile uint32_t suspended_events;
static volatile uint32_t resumed_events;
static volatile uint32_t msc_event_count[5];
static volatile uint32_t msc_last_mount_point;
static volatile bool reset_requested;

static esp_err_t ram_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size, uint64_t src,
                          size_t length)
{
    (void)dev;
    if (src + length > RAMDISK_BYTES || length > dst_size) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dst, ramdisk + src, length);
    read_calls++;
    read_bytes += length;
    return ESP_OK;
}

static esp_err_t ram_write(esp_blockdev_handle_t dev, const uint8_t *source, uint64_t dst,
                           size_t length)
{
    (void)dev;
    if (dst + length > RAMDISK_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(ramdisk + dst, source, length);
    write_calls++;
    write_bytes += length;
    return ESP_OK;
}

static esp_err_t ram_erase(esp_blockdev_handle_t dev, uint64_t start, size_t length)
{
    (void)dev;
    if (start + length > RAMDISK_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ramdisk + start, 0, length);
    return ESP_OK;
}

static esp_err_t ram_sync(esp_blockdev_handle_t dev)
{
    (void)dev;
    return ESP_OK;
}

static esp_err_t ram_ioctl(esp_blockdev_handle_t dev, const uint8_t command, void *arguments)
{
    (void)dev;
    (void)command;
    (void)arguments;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ram_release(esp_blockdev_handle_t dev)
{
    (void)dev;
    return ESP_OK;
}

static const esp_blockdev_ops_t ram_ops = {
    .read = ram_read,
    .write = ram_write,
    .erase = ram_erase,
    .sync = ram_sync,
    .ioctl = ram_ioctl,
    .release = ram_release,
};

static esp_blockdev_t ram_device;

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

static void msc_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *argument)
{
    (void)handle;
    (void)argument;
    if ((unsigned)event->id < 5U) {
        msc_event_count[event->id]++;
    }
    msc_last_mount_point = (uint32_t)event->mount_point;
}

static void cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buffer[32];
    size_t length = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buffer, sizeof(buffer), &length) != ESP_OK) {
        return;
    }
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

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

/* Names and sizes of the files in Inbox, while the application owns the volume. */
static int list_inbox(char *out, size_t capacity)
{
    DIR *directory = opendir(MOUNT_PATH "/Inbox");
    if (directory == NULL) {
        return snprintf(out, capacity, "inbox=unavailable");
    }
    int used = snprintf(out, capacity, "inbox=");
    struct dirent *entry;
    unsigned shown = 0;
    while ((entry = readdir(directory)) != NULL && shown < 4 && used > 0 && (size_t)used < capacity) {
        char path[96];
        struct stat info;
        snprintf(path, sizeof(path), MOUNT_PATH "/Inbox/%.60s", entry->d_name);
        long size = stat(path, &info) == 0 ? (long)info.st_size : -1;
        used += snprintf(out + used, capacity - (size_t)used, "%s%.40s:%ld", shown ? "," : "",
                         entry->d_name, size);
        shown++;
    }
    closedir(directory);
    return used;
}

void app_main(void)
{
    ramdisk = heap_caps_calloc(1, RAMDISK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "ramdisk %p", (void *)ramdisk);
    if (ramdisk == NULL) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ram_device.ctx = NULL;
    ram_device.device_flags.val = 0; /* read/write, no erase before write */
    ram_device.geometry.disk_size = RAMDISK_BYTES;
    ram_device.geometry.read_size = SECTOR_BYTES;
    ram_device.geometry.write_size = SECTOR_BYTES;
    ram_device.geometry.erase_size = SECTOR_BYTES;
    ram_device.ops = &ram_ops;

    const tinyusb_msc_driver_config_t driver_config = {
        .callback = msc_event,
        .callback_arg = NULL,
    };
    esp_err_t driver_error = tinyusb_msc_install_driver(&driver_config);
    const tinyusb_msc_storage_config_t storage_config = {
        .medium.blockdev = &ram_device,
        .fat_fs = {
            .config = {.max_files = 4},
            .do_not_format = false,
            .format_flags = FM_ANY,
            .base_path = MOUNT_PATH,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    tinyusb_msc_storage_handle_t storage = NULL;
    esp_err_t storage_error = tinyusb_msc_new_storage_blockdev(&storage_config, &storage);

    int folders_error = 0;
    if (storage_error == ESP_OK) {
        folders_error |= mkdir(MOUNT_PATH "/Inbox", 0777);
        folders_error |= mkdir(MOUNT_PATH "/Applications", 0777);
        write_text(MOUNT_PATH "/README.TXT", "Roundie MSC probe. Put files in Inbox, then eject.\r\n");
        write_text(MOUNT_PATH "/Applications/LIST.TXT", "org.roundie.watch 1.3.0\r\n");
    }
    esp_err_t usb_error = ESP_FAIL;
    esp_err_t cdc_error = ESP_FAIL;
    if (storage_error == ESP_OK) {
        const tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event, NULL);
        usb_error = tinyusb_driver_install(&config);
        const tinyusb_config_cdcacm_t acm = {.cdc_port = TINYUSB_CDC_ACM_0, .callback_rx = cdc_rx};
        cdc_error = tinyusb_cdcacm_init(&acm);
    }
    ESP_LOGI(TAG, "driver %s storage %s usb %s cdc %s", esp_err_to_name(driver_error),
             esp_err_to_name(storage_error), esp_err_to_name(usb_error), esp_err_to_name(cdc_error));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
        if (storage != NULL) {
            tinyusb_msc_get_storage_mount_point(storage, &mount_point);
        }
        /* The host can vanish while the application owns the volume, so keep the
         * last listing taken in that state and the number of such seconds. */
        static char last_app_inbox[200] = "none";
        static uint32_t app_seconds;
        char inbox[200] = "inbox=host-owned";
        if (mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            list_inbox(inbox, sizeof(inbox));
            snprintf(last_app_inbox, sizeof(last_app_inbox), "%s", inbox);
            app_seconds++;
        }
        char line[640];
        int length = snprintf(line, sizeof(line),
            "MSCPROBE up=%" PRIu32 " init=%d/%d/%d/%d dirs=%d att=%" PRIu32 " det=%" PRIu32
            " susp=%" PRIu32 " res=%" PRIu32 " owner=%s ev=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
            " rd=%" PRIu32 ":%" PRIu64 " wr=%" PRIu32 ":%" PRIu64 " psram_free=%u app_s=%" PRIu32 " last_app_%s | %s\r\n",
            uptime_s, (int)driver_error, (int)storage_error, (int)usb_error, (int)cdc_error, folders_error,
            attached_events, detached_events, suspended_events, resumed_events,
            mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP",
            msc_event_count[0], msc_event_count[1], msc_event_count[2], msc_event_count[3], msc_event_count[4],
            read_calls, read_bytes, write_calls, write_bytes,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), app_seconds, last_app_inbox, inbox);
        if (length > 0 && tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0)) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, (size_t)length);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
        if (reset_requested || uptime_s >= SELF_RESET_AFTER_S) {
            esp_restart();
        }
    }
}
