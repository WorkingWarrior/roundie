/* Include the actual engine to inject retained-record corruption without a
 * firmware-only debug API. Persistence and USB controller are separate actual
 * translation units with mocked IDF APIs. No reference state machine. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../components/roundie_boot_guard/boot_guard.c"
#include "nvs.h"
#include "esp_system.h"
#include "roundie_ota.h"
#include "recovery_diagnostics.h"
#include "roundie_elf_crash.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "roundie_storage_mode.h"

static uint8_t image[32] = {1};
static uint8_t blob[16];
static bool blob_present;
static bool nvs_ok = true;
static bool write_ok = true;
static unsigned writes, commits, resets, selections;
static esp_reset_reason_t raw_reset;
static char response[1200];
static roundie_ota_status_t ota;
static bool elf_busy;
static bool elf_allowed;
static bool finish_during_poll;

void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
bool roundie_app_runtime_busy(void) { return elf_busy; }
void roundie_elf_set_allowed(bool allowed) { elf_allowed = allowed; }
void roundie_app_runtime_poll(int64_t now)
{
    (void)now;
    if (finish_during_poll) {
        elf_busy = false;
    }
}
int64_t esp_timer_get_time(void) { return 100; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t description;
    memcpy(description.app_elf_sha256, image, sizeof(image));
    return &description;
}

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
size_t heap_caps_get_minimum_free_size(unsigned flags) { (void)flags; return 90000; }
size_t heap_caps_get_largest_free_block(unsigned flags) { (void)flags; return 80000; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
static esp_alloc_failed_hook_t failed_alloc_hook;
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)
{
    failed_alloc_hook = callback;
    return ESP_OK;
}
static int known_task;
TaskHandle_t xTaskGetHandle(const char *name)
{
    if (strcmp(name, "main") == 0) {
        return &known_task;
    }
    return NULL;
}
unsigned uxTaskGetNumberOfTasks(void) { return 7; }
unsigned uxTaskGetStackHighWaterMark(void *task)
{
    assert(task == &known_task);
    return 1234;
}
static jmp_buf hang_exit;
static bool hang_expected;
void vTaskDelay(unsigned ticks)
{
    assert(ticks == 100);
    if (hang_expected) {
        longjmp(hang_exit, 1);
    }
}
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
    elf_busy = false;
    elf_allowed = true;
    finish_during_poll = false;
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
    assert(status().safe && status().reason == ROUNDIE_SAFE_CORE);
    const roundie_reset_class_t runtime_faults[] = {ROUNDIE_RESET_PANIC, ROUNDIE_RESET_SOFTWARE};
    for (size_t i = 0; i < sizeof(runtime_faults) / sizeof(runtime_faults[0]); i++) {
        fresh();
        stable();
        start(runtime_faults[i]);
        assert(status().safe && status().reason == ROUNDIE_SAFE_CORE && status().failures == 0);
    }
    fresh();
    stable();
    roundie_boot_planned_restart();
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().safe && status().failures == 0);
    fresh();
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

    /* USB storage mode: STORAGE asks for a planned restart only when the image
     * is VALID and STABLE, OTA is idle and nothing is loaded. */
    fresh();
    stable();
    ota.running.state = ESP_OTA_IMG_VALID;
    ota.phase = ROUNDIE_OTA_IDLE;
    unsigned resets_before = resets;
    elf_busy = true;
    assert(!command("STORAGE", false) && resets == resets_before);
    elf_busy = false;
    ota.phase = ROUNDIE_OTA_RECEIVING;
    assert(!command("STORAGE", false) && resets == resets_before);
    ota.phase = ROUNDIE_OTA_IDLE;
    ota.running.state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(!command("STORAGE", false) && resets == resets_before);
    ota.running.state = ESP_OTA_IMG_VALID;
    assert(!command("STORAGE", true) && resets == resets_before);
    assert(command("STORAGE", false) && resets == resets_before + 1);
    start(ROUNDIE_RESET_SOFTWARE);
    assert(roundie_storage_mode_begin(true, image) && roundie_storage_mode_active());
    assert(!command("STORAGE", false) && !roundie_storage_mode_begin(true, image));

    fresh();
    assert(command("STATUS", false));
    assert(strstr(response, "\"tasks\"") == NULL && strstr(response, "\"storage\":0}\n") != NULL);
    roundie_recovery_health_init();
    assert(failed_alloc_hook != NULL);
    failed_alloc_hook(64, MALLOC_CAP_SPIRAM, "test");
    assert(command("HEALTH", false));
    assert(strstr(response, "\"internal_min\":90000,\"internal_largest\":80000") != NULL);
    assert(strstr(response, "\"alloc_failed\":1,\"alloc_failed_size\":64,\"alloc_failed_caps\":4,\"tasks\":7") != NULL);
    assert(strstr(response, "\"stacks\":{\"main\":1234,\"system_worker\":-1,") != NULL);
    assert(strstr(response, "\"IDLE1\":-1}}\n") != NULL);
    assert(!command("HEALTH extra", false));
    assert(command("CRASH", false) && strcmp(response, "\n!RS1 1234abcd 0 {\"crash\":{\"valid\":0}}\n") == 0);
    roundie_elf_crash_boot();
    roundie_elf_crash_context_t crash = {.instance = 7, .text_exec = 0x42000000,
                                         .sections = {{0, 0x3c000000, 0x100}}};
    memset(crash.digest, 0xab, sizeof(crash.digest));
    roundie_elf_crash_loaded(&crash);
    roundie_elf_crash_fault(&(roundie_elf_crash_fault_t){.pc = 0x42000010, .exccause = 28, .excvaddr = 4,
        .sp = 0x3fc90000, .exception = 4, .depth = 2, .frames = {0x42000010, 0x40380000}});
    roundie_elf_crash_boot(); /* the restart after the panic */
    assert(command("CRASH", false));
    assert(strstr(response, "{\"crash\":{\"valid\":1,\"loaded\":1,\"fault\":1,\"instance\":7,\"digest\":\"abababab") != NULL);
    assert(strstr(response, "\"text_exec\":1107296256,\"sections\":[[0,1006632960,256],[0,0,0],[0,0,0],[0,0,0],[0,0,0]],"
                  "\"pc\":1107296272,\"cause\":28,\"vaddr\":4,\"sp\":1070137344,\"exception\":4,\"core\":0,"
                  "\"frames\":[1107296272,1077411840]}}\n") != NULL);
    assert(!command("CRASH extra", false));
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
    fresh();
    stable();
    elf_busy = true;
    assert(command("REQUEST", false));
    assert(status().safe && !elf_allowed && resets == 0 && writes == 0 && commits == 0);
    assert(!command("CLEAR", false) && !command("REBOOT", false));
    roundie_recovery_poll(100);
    assert(writes == 0 && commits == 0 && resets == 0);
    elf_busy = false; /* Callback returned and executor completed teardown. */
    roundie_recovery_poll(100);
    assert(writes == 1 && commits == 1 && resets == 1);
    fresh();
    stable();
    elf_busy = true;
    finish_during_poll = true; /* Teardown races REQUEST before its reply. */
    assert(command("REQUEST", false));
    assert(!elf_busy && writes == 0 && resets == 0);
    roundie_recovery_poll(100);
    assert(writes == 1 && commits == 1 && resets == 1);
    char hang[80];
    fresh();
    snprintf(hang, sizeof(hang), "HANG %s", ota.running.elf);
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
    /* Faults must also be reachable before STABLE and inside Safe Mode. */
    for (int state = 0; state < 3; state++) {
        fresh();
        if (state >= 1) {
            stable();
        }
        if (state == 2) {
            assert(command("REQUEST", false));
        }
        hang_expected = true;
        if (setjmp(hang_exit) == 0) {
            command(hang, false);
            assert(false);
        }
        hang_expected = false;
        assert(strstr(response, "!RS1 1234abcd 0 ") != NULL && strstr(response, "\"restart\":1") != NULL);
    }
    fresh();
    stable();
    assert(resets == 0);
    char wrong[80];
    snprintf(wrong, sizeof(wrong), "HANG %064d", 0);
    assert(!command(wrong, false));
#else
    stable();
    assert(!command(hang, false));
#endif
    /* A retained GUI suppression must not defeat the explicit normal trial;
     * a trial that fails in GUI suppresses it again. */
    fresh();
    stable();
    start(ROUNDIE_RESET_WATCHDOG);
    assert(status().safe && !status().skip_gui);
    start(ROUNDIE_RESET_PANIC);
    assert(status().safe && status().skip_gui);
    assert(roundie_boot_normal_trial() && roundie_boot_persistence_sync());
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().safe && status().trial && !status().skip_gui);
    roundie_boot_stage(ROUNDIE_BOOT_GUI);
    start(ROUNDIE_RESET_PANIC);
    assert(status().safe && status().skip_gui && status().reason == ROUNDIE_SAFE_BOOT_LOOP);
    /* A failed session names its loaded package for exactly the next boot. */
    fresh();
    stable();
    roundie_boot_app_loaded("org.roundie.ui");
    start(ROUNDIE_RESET_WATCHDOG);
    assert(status().safe && status().reason == ROUNDIE_SAFE_CORE);
    assert(strcmp(status().fault_app, "org.roundie.ui") == 0);
    assert(command("STATUS", false) && strstr(response, "\"fault_app\":\"org.roundie.ui\"") != NULL);
    roundie_boot_planned_restart();
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().fault_app[0] == '\0');
    fresh();
    stable();
    roundie_boot_app_loaded("org.roundie.ui");
    roundie_boot_app_loaded("");
    start(ROUNDIE_RESET_PANIC);
    assert(status().safe && status().fault_app[0] == '\0');
    fresh();
    stable();
    roundie_boot_app_loaded("org.roundie.ui");
    roundie_boot_planned_restart();
    start(ROUNDIE_RESET_SOFTWARE);
    assert(!status().safe && status().fault_app[0] == '\0');
    fresh();
    stable();
    roundie_boot_app_loaded("org.roundie.ui");
    start(ROUNDIE_RESET_POWER);
    assert(status().fault_app[0] == '\0');
    fresh();
    stable();
    char longest[ROUNDIE_BOOT_APP_ID + 8];
    memset(longest, 'a', sizeof(longest) - 1);
    longest[sizeof(longest) - 1] = '\0';
    roundie_boot_app_loaded(longest);
    start(ROUNDIE_RESET_PANIC);
    assert(status().retained_valid && strlen(status().fault_app) == ROUNDIE_BOOT_APP_ID - 1);
    for (int reason = ESP_RST_UNKNOWN; reason <= ESP_RST_CPU_LOCKUP; reason++) {
        raw_reset = (esp_reset_reason_t)reason;
        assert(roundie_boot_reset_class() <= ROUNDIE_RESET_EXTERNAL);
    }
    puts("BootGuard engine, actual NVS backend and RS1 controller PASS (mocked IDF, no hardware)");
    return 0;
}
