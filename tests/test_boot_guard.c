/* Include the actual engine to inject retained-record corruption without a
 * firmware-only debug API. Persistence and USB controller are separate actual
 * translation units with mocked IDF APIs. No reference state machine. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../components/roundie_boot_guard/boot_guard.c"
#include "nvs.h"
#include "esp_system.h"
#include "roundie_ota.h"
#include "recovery_diagnostics.h"

static uint8_t image[32] = {1};
static uint8_t blob[16];
static bool blob_present;
static bool nvs_ok = true;
static bool write_ok = true;
static unsigned writes, commits, resets, selections;
static esp_reset_reason_t raw_reset;
static char response[1200];
static roundie_ota_status_t ota;

esp_err_t nvs_open(const char *name, unsigned mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "boot_guard") == 0 && mode == NVS_READWRITE);
    *handle = 1;
    return nvs_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    assert(handle == 1 && strcmp(key, "latch") == 0 && *size == sizeof(blob));
    if (!blob_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(data, blob, sizeof(blob));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1 && strcmp(key, "latch") == 0 && size == sizeof(blob));
    writes++;
    if (!write_ok) {
        return ESP_FAIL;
    }
    memcpy(blob, data, size);
    blob_present = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    commits++;
    return write_ok ? ESP_OK : ESP_FAIL;
}
esp_reset_reason_t esp_reset_reason(void) { return raw_reset; }
void esp_restart(void) { resets++; }
size_t heap_caps_get_free_size(unsigned flags) { (void)flags; return 100000; }
void vTaskDelay(unsigned ticks) { assert(ticks == 100); }
esp_err_t usb_serial_jtag_wait_tx_done(unsigned ticks) { assert(ticks == 100); return ESP_OK; }
int usb_serial_jtag_write_bytes(const void *data, size_t size, unsigned ticks)
{
    assert(ticks == 100 && size < sizeof(response));
    memcpy(response, data, size);
    response[size] = '\0';
    return (int)size;
}
void roundie_ota_status(roundie_ota_status_t *out) { *out = ota; }
const char *roundie_ota_image_state_name(int state) { return state == ESP_OTA_IMG_VALID ? "VALID" : "OTHER"; }
esp_err_t roundie_ota_select_valid(const char *label, const char *elf)
{
    assert(strcmp(label, "ota_0") == 0 && strlen(elf) == 64);
    selections++;
    return ESP_OK;
}

static roundie_boot_status_t status(void)
{
    roundie_boot_status_t result;
    roundie_boot_status(&result);
    return result;
}
static void start(roundie_reset_class_t reset)
{
    roundie_boot_begin(reset, image);
    roundie_boot_persistence_init(nvs_ok);
}
static void fresh(void)
{
    blob_present = false;
    nvs_ok = write_ok = true;
    writes = commits = resets = selections = 0;
    memset(blob, 0, sizeof(blob));
    start(ROUNDIE_RESET_POWER);
    ota = (roundie_ota_status_t){.initialized = true, .boot_checked = true, .boot_passed = true};
    ota.running.state = ESP_OTA_IMG_VALID;
    strcpy(ota.running.label, "ota_1");
    memset(ota.running.elf, 'a', 64);
}
static void stable(void)
{
    roundie_boot_operational(100, true);
    roundie_boot_tick(100 + ROUNDIE_BOOT_STABILITY_US - 1);
    assert(status().stage != ROUNDIE_BOOT_STABLE);
    roundie_boot_tick(100 + ROUNDIE_BOOT_STABILITY_US);
    assert(status().stage == ROUNDIE_BOOT_STABLE);
}
static bool command(const char *operation, bool discard)
{
    char line[1100];
    snprintf(line, sizeof(line), "!RS1 1234abcd %s", operation);
    roundie_recovery_command(line, discard);
    return strstr(response, "!RS1 1234abcd 0 ") != NULL;
}

int main(void)
{
    fresh();
    assert(!status().safe && !status().retained_valid && status().failures == 0);
    roundie_boot_operational(0, false);
    roundie_boot_tick(ROUNDIE_BOOT_STABILITY_US + 1);
    assert(status().stage == ROUNDIE_BOOT_START);
    stable();
    assert(roundie_boot_persistence_sync() && writes == 0);
    start(ROUNDIE_RESET_WATCHDOG);
    assert(status().failures == 0); /* Stable runtime fault is not a startup loop. */
    for (unsigned i = 0; i < 4; i++) {
        roundie_boot_planned_restart(); /* Ordinary and OTA activation restart. */
        start(ROUNDIE_RESET_SOFTWARE);
        assert(!status().safe && status().failures == 0);
    }
    start(ROUNDIE_RESET_PANIC);
    assert(status().failures == 1 && !status().safe);
    start(ROUNDIE_RESET_WATCHDOG);
    assert(status().failures == 2 && !status().safe);
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().failures == 3 && status().safe && status().reason == ROUNDIE_SAFE_BOOT_LOOP);
    assert(roundie_boot_persistence_sync() && writes == 1);
    for (unsigned i = 0; i < 5; i++) {
        start(ROUNDIE_RESET_PANIC);
        assert(status().safe && status().failures == 3 && status().skip_gui);
        assert(roundie_boot_persistence_sync() && writes == 1);
    }
    start(ROUNDIE_RESET_POWER);
    assert(status().safe && status().failures == 0); /* Durable latch, no retained crash count. */
    assert(roundie_boot_normal_trial() && roundie_boot_persistence_sync());
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().safe && status().trial);
    start(ROUNDIE_RESET_SOFTWARE); /* Failed trial immediately returns to recovery. */
    assert(status().safe && !status().trial);
    assert(roundie_boot_normal_trial());
    start(ROUNDIE_RESET_SOFTWARE);
    stable();
    assert(!status().safe && status().latch == 0 && status().failures == 0);
    assert(roundie_boot_persistence_sync());
    start(ROUNDIE_RESET_POWER);
    assert(!status().safe);

    for (unsigned reason = ROUNDIE_RESET_UNKNOWN; reason <= ROUNDIE_RESET_EXTERNAL; reason++) {
        if (reason == ROUNDIE_RESET_PANIC || reason == ROUNDIE_RESET_WATCHDOG || reason == ROUNDIE_RESET_SOFTWARE) {
            continue;
        }
        fresh();
        start((roundie_reset_class_t)reason);
        assert(status().failures == 0 && !status().safe);
    }
    fresh();
    start(ROUNDIE_RESET_PANIC);
    stable();
    assert(status().failures == 0);
    retained.crc ^= 1;
    start(ROUNDIE_RESET_PANIC);
    assert(!status().retained_valid && status().failures == 0);
    retained.version++;
    retained.crc = checksum();
    start(ROUNDIE_RESET_PANIC);
    assert(!status().retained_valid);
    image[0]++;
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().retained_valid && status().failures == 0);
    blob_present = true;
    start(ROUNDIE_RESET_POWER);
    assert(status().safe && status().reason == ROUNDIE_SAFE_PERSISTENCE);
    assert(roundie_boot_clear() && roundie_boot_persistence_sync());
    start(ROUNDIE_RESET_POWER);
    assert(!status().safe);

    fresh();
    nvs_ok = false;
    start(ROUNDIE_RESET_POWER);
    assert(status().safe && !status().persistence_ok && !roundie_boot_clear() && !roundie_boot_normal_trial());
    fresh();
    roundie_boot_enter_safe(ROUNDIE_SAFE_STORAGE);
    write_ok = false;
    assert(!roundie_boot_persistence_sync() && status().safe && !status().persistence_ok);
    fresh();
    roundie_boot_enter_safe(ROUNDIE_SAFE_STORAGE);
    assert(status().safe && !status().skip_gui);
    assert(roundie_recovery_confirmation_allowed(ESP_OTA_IMG_PENDING_VERIFY));
    roundie_boot_enter_safe(ROUNDIE_SAFE_GUI);
    assert(status().skip_gui && status().reason == ROUNDIE_SAFE_STORAGE);
    /* Caller still must supply real GUI/DMA health; this helper alone never
     * calls the OTA confirmation API. */

    fresh();
    assert(command("STATUS", false));
    assert(!command("REQUEST extra", false) && !status().safe);
    assert(!command("REQUEST", true) && !status().safe);
    ota.running.state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(!command("REQUEST", false) && !command("REBOOT", false));
    assert(!command("CLEAR", false) && resets == 0);
    ota.running.state = ESP_OTA_IMG_VALID;
    ota.phase = ROUNDIE_OTA_RECEIVING;
    assert(!command("REQUEST", false));
    ota.phase = ROUNDIE_OTA_IDLE;
    assert(command("REQUEST", false) && resets == 1);
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().safe && status().failures == 0);
    assert(!roundie_recovery_confirmation_allowed(ESP_OTA_IMG_PENDING_VERIFY));
    assert(roundie_recovery_confirmation_allowed(ESP_OTA_IMG_VALID));
    ota.boot_passed = false; /* Headless recovery can reboot a VALID image. */
    assert(command("STATUS", false));
    assert(command("SLOT STATUS", false));
    assert(command("CLEAR", false) && status().safe && status().latch == 0);
    roundie_recovery_poll(100);
    assert(status().latch == 0); /* Failed GUI must not undo explicit CLEAR. */
    assert(command("REBOOT", false) && resets == 2);
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().safe && status().trial);
    stable();
    assert(!status().trial);
    assert(roundie_boot_persistence_sync());

    fresh();
    stable();
    char fault[80];
    snprintf(fault, sizeof(fault), "FAULT %s", ota.running.elf);
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
    assert(command(fault, false) && resets == 1 && status().fault_remaining == 2);
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().failures == 1 && roundie_boot_fault_step(true));
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().failures == 2 && !roundie_boot_fault_step(false));
    assert(roundie_boot_fault_step(true));
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().safe && status().failures == 3 && status().fault_remaining == 0);
    assert(!roundie_boot_fault_step(true) && !roundie_boot_fault_arm());
#else
    assert(!command(fault, false) && resets == 0 && !roundie_boot_fault_arm());
    assert(!roundie_boot_fault_step(true));
#endif
    for (int reason = ESP_RST_UNKNOWN; reason <= ESP_RST_CPU_LOCKUP; reason++) {
        raw_reset = (esp_reset_reason_t)reason;
        assert(roundie_boot_reset_class() <= ROUNDIE_RESET_EXTERNAL);
    }
    puts("BootGuard engine, actual NVS backend and RS1 controller PASS (mocked IDF, no hardware)");
    return 0;
}
