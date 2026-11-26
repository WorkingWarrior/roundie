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
static uint8_t cause_blob[512];
static size_t cause_size;
static unsigned cause_writes, cause_commits;
static bool cause_pending;
static uint8_t crash_blob[512];
static size_t crash_size;
static unsigned crash_writes, crash_commits;
static bool crash_pending;
static size_t nvs_available = 378; /* nvs_get_stats available_entries */
/* Every latch record written, in order: identical for every CAUSE_FAULT mode. */
static char latch_trace[512];
/* CAUSE_FAULT: 0 none, 1 cause set fails (NVS full), 2 damaged cause entry
 * (reads back garbage, set fails), 3 cause commit fails. */
#ifndef CAUSE_FAULT
#define CAUSE_FAULT 0
#endif
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
static int64_t clock_us = 100;
int64_t esp_timer_get_time(void) { return clock_us; }
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
    if (strcmp(key, "crash") == 0) {
        assert(handle == 1 && *size <= sizeof(crash_blob));
        if (crash_size == 0) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        if (*size != crash_size) {
            return ESP_ERR_NVS_INVALID_LENGTH;
        }
        memcpy(data, crash_blob, crash_size);
        return ESP_OK;
    }
    if (strcmp(key, "cause") == 0) {
        assert(handle == 1 && *size <= sizeof(cause_blob));
        if (CAUSE_FAULT == 2) {
            memset(data, 0x5a, *size);
            return ESP_OK;
        }
        if (cause_size == 0) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        assert(*size == cause_size);
        memcpy(data, cause_blob, cause_size);
        return ESP_OK;
    }
    assert(handle == 1 && strcmp(key, "latch") == 0 && *size == sizeof(blob));
    if (!blob_present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(data, blob, sizeof(blob));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    if (strcmp(key, "crash") == 0) {
        assert(handle == 1 && size <= sizeof(crash_blob));
        crash_writes++;
        memcpy(crash_blob, data, size);
        crash_size = size;
        crash_pending = true;
        return ESP_OK;
    }
    if (strcmp(key, "cause") == 0) {
        assert(handle == 1 && size <= sizeof(cause_blob));
        cause_writes++;
        if (CAUSE_FAULT == 1) {
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        if (CAUSE_FAULT == 2 || !write_ok) {
            return ESP_FAIL;
        }
        cause_pending = true;
        memcpy(cause_blob, data, size);
        cause_size = size;
        return ESP_OK;
    }
    assert(handle == 1 && strcmp(key, "latch") == 0 && size == sizeof(blob));
    assert(!cause_pending); /* The latch never shares a commit with the cause. */
    writes++;
    char entry[32];
    snprintf(entry, sizeof(entry), "%u%s ", ((const uint32_t *)data)[2], write_ok ? "" : "!");
    strncat(latch_trace, entry, sizeof(latch_trace) - strlen(latch_trace) - 1);
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
    if (crash_pending) {
        crash_pending = false;
        crash_commits++;
        return ESP_OK;
    }
    if (cause_pending) {
        cause_pending = false;
        cause_commits++;
        return CAUSE_FAULT == 3 ? ESP_FAIL : ESP_OK;
    }
    commits++;
    return write_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *stats)
{
    assert(part_name == NULL);
    stats->total_entries = 504;
    stats->available_entries = nvs_available;
    stats->used_entries = 378 - nvs_available;
    stats->free_entries = 504 - stats->used_entries;
    stats->namespace_count = 4;
    return ESP_OK;
}
esp_reset_reason_t esp_reset_reason(void) { return raw_reset; }
/* The boot history has its own test (test_boot_history.c). */
void roundie_boot_history_get(roundie_boot_history_t *out) { memset(out, 0, sizeof(*out)); }
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
    roundie_boot_diag_begin(reset == ROUNDIE_RESET_POWER ? ESP_RST_POWERON : ESP_RST_SW, image);
    roundie_boot_persistence_init(nvs_ok);
}
static void fresh(void)
{
    blob_present = false;
    cause_size = 0;
    cause_writes = cause_commits = 0;
    cause_pending = false;
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
    roundie_boot_cause_t cause;
    roundie_boot_diag_stored_cause(&cause);
    assert(!cause.valid);
    assert(roundie_boot_persistence_sync() && writes == 1 && commits == 1);
    /* The cause record follows in its own commit and is diagnostic only. */
    assert(cause_writes == 1);
    roundie_boot_diag_stored_cause(&cause);
#if CAUSE_FAULT == 0
    assert(cause_commits == 1);
    assert(cause.valid && cause.latch == ROUNDIE_SAFE_BOOT_LOOP && cause.site == ROUNDIE_SAFE_SITE_RETAINED);
    assert(cause.raw_reset == ESP_RST_SW && cause.failures == 3 && cause.retained_valid);
    assert(memcmp(cause.image, image, sizeof(cause.image)) == 0);
#else
    assert(!cause.valid); /* A failed diagnostic write is simply absent. */
#endif
    for (unsigned i = 0; i < 5; i++) {
        start(ROUNDIE_RESET_PANIC);
        assert(status().safe && status().failures == 3 && status().skip_gui);
        assert(roundie_boot_persistence_sync() && writes == 1);
    }
    start(ROUNDIE_RESET_POWER);
    assert(status().safe && status().failures == 0); /* Durable latch, no retained crash count. */
    roundie_boot_diag_stored_cause(&cause); /* Reloaded from NVS after power-on. */
#if CAUSE_FAULT == 0 || CAUSE_FAULT == 3
    /* nvs_set_blob() already wrote the entry; only its commit call failed. */
    assert(cause.valid && cause.latch == ROUNDIE_SAFE_BOOT_LOOP && cause.failures == 3);
#else
    assert(!cause.valid); /* Damaged or missing entry: ignored, never fatal. */
#endif
    roundie_boot_session_t previous;
    roundie_boot_diag_previous(&previous);
    assert(!previous.valid); /* RTC RAM is not trusted after power-on. */
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
    assert(strstr(response, "\"IDLE1\":-1},\"executor_guard\":{\"armed\":0,\"limit\":0,\"usable_min\":-1,\"spurious\":0}}\n") != NULL);
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
                  "\"frames\":[1107296272,1077411840],\"frame\":0,\"phase\":\"idle\",\"trigger\":0,"
                  "\"monitor_pc\":0,\"stack\":[0,0,0,0,0],\"origin\":\"app_code\"}}\n") != NULL);
    assert(!command("CRASH extra", false));
    /* NVS copy of the crash record (E0 §9.4 item 3): bounded, own commit,
     * yields to the reserve kept for settings, touches no other key. */
    assert(command("CRASH STORED", false));
    assert(strstr(response, "{\"crash\":{\"stored\":1,\"store\":-1,\"limit\":256,\"size\":224,"
                  "\"nvs\":[1,0,378,504],\"raw_reset\":0,\"image\":\"\",\"valid\":0}}\n") != NULL);
    roundie_recovery_store_crash(ESP_RST_SW, image); /* not a fault reset: nothing to copy */
    assert(crash_writes == 0);
    unsigned latch_writes = writes;
    unsigned latch_commits = commits;
    uint8_t latch_before[sizeof(blob)];
    memcpy(latch_before, blob, sizeof(blob));
    nvs_available = roundie_boot_diag_entries(224) + ROUNDIE_BOOT_DIAG_RESERVE_ENTRIES - 1; /* full */
    roundie_recovery_store_crash(ESP_RST_PANIC, image);
    assert(crash_writes == 0 && crash_commits == 0);
    assert(command("CRASH STORED", false) && strstr(response, "\"store\":3,") != NULL);
    assert(strstr(response, "\"valid\":0}}") != NULL);
    nvs_available = roundie_boot_diag_entries(224) + ROUNDIE_BOOT_DIAG_RESERVE_ENTRIES;
    roundie_recovery_store_crash(ESP_RST_PANIC, image);
    assert(crash_writes == 1 && crash_commits == 1);
    assert(command("CRASH STORED", false) && strstr(response, "\"store\":0,") != NULL);
    char expected_image[64];
    snprintf(expected_image, sizeof(expected_image), "\"raw_reset\":4,\"image\":\"%02x00000000000000\",", image[0]);
    assert(strstr(response, expected_image) != NULL);
    assert(strstr(response, "\",\"valid\":1,\"loaded\":1,\"fault\":1,\"instance\":7,\"digest\":\"abababab") != NULL);
    assert(strstr(response, "\"origin\":\"app_code\"}}\n") != NULL);
    roundie_recovery_store_crash(ESP_RST_TASK_WDT, image); /* identical apart from the reset */
    assert(crash_writes == 2);
    roundie_recovery_store_crash(ESP_RST_TASK_WDT, image); /* identical copy: no write */
    assert(crash_writes == 2 && crash_commits == 2);
    assert(command("CRASH STORED", false) && strstr(response, "\"store\":1,") != NULL);
    crash_blob[20] ^= 1U; /* damaged copy: reported absent, never fatal */
    assert(command("CRASH STORED", false) && strstr(response, "\"valid\":0}}") != NULL);
    uint8_t oversized[ROUNDIE_BOOT_DIAG_BLOB_MAX + 1] = {0};
    assert(roundie_boot_diag_store("crash", oversized, sizeof(oversized)) == ROUNDIE_DIAG_TOO_LARGE);
    assert(writes == latch_writes && commits == latch_commits && memcmp(blob, latch_before, sizeof(blob)) == 0);
    nvs_available = 378;
    /* RTC session record (§9.4 items 2 and 4): restart site and battery
     * survive a software/watchdog reset, never a power-on. */
    roundie_boot_diag_restart_site(ROUNDIE_RESTART_SITE_GUI_WATCHDOG, 5000000);
    roundie_boot_diag_battery(3900, -120, 80, 0x10, 251, 1764000000, 6000000);
    roundie_boot_diag_battery(3700, -100, 70, 0, 250, 1764000001, 7000000); /* within 60 s: kept out */
    roundie_boot_diag_begin(ESP_RST_TASK_WDT, image);
    roundie_boot_session_t session;
    roundie_boot_diag_previous(&session);
    assert(session.valid && session.restart_site == ROUNDIE_RESTART_SITE_GUI_WATCHDOG && session.voltage_mv == 3900);
    assert(command("CAUSE", false));
    assert(strstr(response, "{\"cause\":{\"valid\":") != NULL);
    assert(strstr(response, "},\"previous\":{\"valid\":1,") != NULL);
    assert(strstr(response, "\"restart_site\":3,\"uptime_ms\":6000,\"battery\":1,\"mv\":3900,\"ma\":-120,"
                  "\"soc\":80,\"flags\":16,\"temp_dc\":251,\"battery_ms\":6000,\"rtc\":1764000000}}\n") != NULL);
    assert(!command("CAUSE extra", false));
    roundie_boot_diag_restart_site(ROUNDIE_RESTART_SITE_LCD_FLUSH, 1000);
    roundie_boot_diag_begin(ESP_RST_POWERON, image); /* RTC RAM undefined: ignored */
    roundie_boot_diag_previous(&session);
    assert(!session.valid);
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
    finish_during_poll = true; /* Teardown finishes right after REQUEST. */
    assert(command("REQUEST", false));
    /* system_worker never runs the teardown itself: that is io_worker's
     * VFS work (docs/storage-io.md), so REQUEST leaves it pending. */
    assert(elf_busy && writes == 0 && resets == 0);
    roundie_app_runtime_poll(100); /* io_worker's next poll. */
    assert(!elf_busy && writes == 0 && resets == 0);
    roundie_recovery_poll(100);
    assert(writes == 1 && commits == 1 && resets == 1);
    /* io_worker blocked: the teardown is never confirmed. Safe Mode must not
     * depend on it: at the deadline system_worker restarts with the request
     * sealed in the RTC record, without an NVS write while the app may run. */
    fresh();
    stable();
    elf_busy = true;
    clock_us = 1000;
    assert(command("REQUEST", false) && status().safe && resets == 0);
    clock_us = 2000; /* A repeated REQUEST must not move the deadline. */
    assert(command("REQUEST", false));
    roundie_recovery_poll(1000 + ROUNDIE_SAFE_TEARDOWN_TIMEOUT_US - 1);
    assert(resets == 0 && writes == 0);
    roundie_recovery_poll(1000 + ROUNDIE_SAFE_TEARDOWN_TIMEOUT_US);
    assert(resets == 1 && writes == 0 && commits == 0);
    clock_us = 100;
    start(ROUNDIE_RESET_SOFTWARE);
    assert(status().safe && status().reason == ROUNDIE_SAFE_REQUEST && status().fault_app[0] == '\0');
    elf_busy = false;
    roundie_recovery_poll(100); /* Safe Mode, no deferred restart pending. */
    assert(resets == 1);
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
    printf("latch trace: %s\n", latch_trace);
    puts("BootGuard engine, actual NVS backend and RS1 controller PASS (mocked IDF, no hardware)");
    return 0;
}
