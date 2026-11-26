#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/elf_diagnostics.c"
#include "esp_ota_ops.h"
#include "roundie_boot_guard.h"
#include "roundie_ota.h"
#include "roundie_watchface.h"
#ifdef ROUNDIE_IO_READ
#include "fake_vfs.h"
#include "roundie_bounded_io.h"
#endif

static roundie_ota_status_t ota;
static roundie_boot_status_t boot;
static bool mounted = true;
static bool app_busy;
static bool package_busy;
bool roundie_packages_busy(void) { return package_busy; }
static bool allow;
static int64_t clock_us = 100000;
static char response[2048];
static unsigned submissions;
static unsigned heap_live;
static uint8_t fixture[ROUNDIE_ELF_MAX_FILE];
static size_t fixture_size;
void roundie_ui_status(roundie_ui_status_t *out) { memset(out, 0, sizeof(*out)); }
bool roundie_ui_input(uint32_t generation, uint32_t widget, uint32_t origin)
{ (void)generation; (void)widget; (void)origin; return false; }
bool roundie_ui_inject_value(uint32_t generation, uint32_t widget, int32_t value)
{ (void)generation; (void)widget; (void)value; return false; }
static char digest_hex[65];
void roundie_elf_budget(roundie_elf_budget_t *instance, roundie_elf_budget_t *boot)
{
    roundie_elf_budget_t counters = {42, 9, {1, 2, 3, 2, 1}, 150000, 60000, 8, 2, 1, 3, 4294967295U};
    if (instance != NULL) {
        *instance = counters;
    }
    if (boot != NULL) {
        counters.callbacks = 4294967295U;
        *boot = counters;
    }
}

int64_t esp_timer_get_time(void) { return clock_us; }
void roundie_ota_status(roundie_ota_status_t *out) { *out = ota; }
void roundie_boot_status(roundie_boot_status_t *out) { *out = boot; }
bool roundie_storage_available(void) { return mounted; }
bool roundie_storage_usage(size_t *total, size_t *used)
{ *total = 0; *used = 0; return false; }
void roundie_packages_poll(int64_t now_us) { (void)now_us; }
void roundie_watchface_poll(int64_t now_us) { (void)now_us; }
void roundie_watchface_init(const roundie_watchface_store_platform_t *platform, bool fallback_only)
{ (void)platform; (void)fallback_only; }
void roundie_boot_app_loaded(const char *id) { (void)id; }
bool roundie_elf_busy(void) { return app_busy; }
void roundie_elf_set_allowed(bool value) { allow = value; }
void roundie_elf_status(roundie_elf_status_t *out) { memset(out, 0, sizeof(*out)); }
void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    (void)caps;
    void *memory = malloc(bytes);
    if (memory != NULL) {
        heap_live++;
    }
    return memory;
}
void heap_caps_free(void *memory)
{
    if (memory != NULL) {
        heap_live--;
        free(memory);
    }
}
esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size)
{
    assert(allow && !app_busy && size == fixture_size && memcmp(bytes, fixture, size) == 0);
    submissions++;
    heap_caps_free(bytes);
    return ESP_OK;
}
esp_err_t roundie_elf_start(uint8_t *bytes, size_t size)
{
    return roundie_elf_submit(bytes, size);
}
esp_err_t roundie_elf_control(uint32_t instance, roundie_elf_control_t operation)
{
    (void)operation;
    return allow && instance == 42 ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t roundie_elf_post(uint32_t instance, uint32_t value)
{
    (void)value;
    return allow && instance == 42 ? ESP_OK : ESP_ERR_INVALID_STATE;
}
int usb_serial_jtag_write_bytes(const void *bytes, size_t length, TickType_t timeout)
{
    assert(timeout == 100 && length < sizeof(response));
    memcpy(response, bytes, length);
    response[length] = 0;
    return (int)length;
}

static int command(const char *format, ...)
{
    char line[1100];
    memcpy(line, "!RE1 12345678 ", 14);
    va_list args;
    va_start(args, format);
    vsnprintf(line + 14, sizeof(line) - 14, format, args);
    va_end(args);
    roundie_elf_command(line, false);
    int result = -1;
    assert(sscanf(response, "\n!RE1 12345678 %d", &result) == 1);
    return result;
}

static void digest(const uint8_t *data, size_t size, char hex[65])
{
    uint8_t bytes[32];
    size_t output;
    assert(psa_hash_compute(PSA_ALG_SHA_256, data, size, bytes, sizeof(bytes), &output) == 0);
    for (unsigned i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", bytes[i]);
    }
}

static void send_file(const uint8_t *bytes, const char *hex)
{
    assert(command("BEGIN 12345678 %zu %s", fixture_size, hex) == ESP_OK);
    assert(roundie_app_runtime_busy());
    assert(command("RUN %s", hex) == ESP_ERR_INVALID_STATE);
    assert(command("DATA 87654321 0 00") == ESP_ERR_INVALID_STATE);
    assert(command("DATA 12345678 1 00") == ESP_ERR_INVALID_SIZE);
    for (size_t i = 0; i < fixture_size; i += 512) {
        size_t count = fixture_size - i > 512 ? 512 : fixture_size - i;
        char text[1025];
        for (size_t j = 0; j < count; j++) {
            snprintf(text + j * 2, 3, "%02x", bytes[i + j]);
        }
        assert(command("DATA 12345678 %zu %s", i, text) == ESP_OK);
    }
}

static void assert_staged_original(void)
{
    FILE *source = fopen(STAGED_ELF, "rb");
    assert(source != NULL);
    uint8_t bytes[ROUNDIE_ELF_MAX_FILE];
    assert(fread(bytes, 1, sizeof(bytes), source) == fixture_size);
    assert(fclose(source) == 0 && memcmp(bytes, fixture, fixture_size) == 0);
    assert(heap_live == 0 && file == -1 && session == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    FILE *source = fopen(argv[1], "rb");
    assert(source != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), source);
    assert(fclose(source) == 0);
    digest(fixture, fixture_size, digest_hex);
    ota.initialized = true;
    ota.boot_checked = true;
    ota.boot_passed = true;
    ota.running.state = ESP_OTA_IMG_VALID;
    strcpy(ota.running.label, "ota_1");
    strcpy(ota.boot, "ota_1");
    boot.stage = ROUNDIE_BOOT_STABLE;
    assert(command("STATUS") == ESP_OK && allow);
    assert(command("RESOURCES") == ESP_OK);
    assert(command("BUDGET") == ESP_OK);
    assert(strcmp(response, "\n!RE1 12345678 0 {\"budget\":[42,9,1,2,3,2,1,150000,60000,8,2,1,3,4294967295],"
                  "\"boot\":[42,4294967295,1,2,3,2,1,150000,60000,8,2,1,3,4294967295]}\n") == 0);
    assert(command("BUDGET x") != ESP_OK); /* Only the exact command reads counters. */
    assert(command("EVENT 42 4294967295") == ESP_OK);
    assert(command("EVENT 42 4294967296") == ESP_ERR_INVALID_ARG);
    assert(command("EVENT 4294967296 1") == ESP_ERR_INVALID_ARG);
    assert(command("EVENT 0 1") == ESP_ERR_INVALID_ARG);
    assert(command("EVENT 42 -1") == ESP_ERR_INVALID_ARG);
    assert(command("EVENT 42 1 extra") == ESP_ERR_INVALID_ARG);
    assert(command("EVENT  42 1") == ESP_ERR_INVALID_ARG);
    assert(command("SUSPEND 42") == ESP_OK);
    assert(command("RESUME 42 ") == ESP_ERR_INVALID_ARG);
    assert(command("UNLOAD 0") == ESP_ERR_INVALID_ARG);
    assert(command("STOP 41") == ESP_ERR_INVALID_STATE);
    boot.safe = true;
    assert(command("RUN %s", digest_hex) == ESP_ERR_INVALID_STATE && !allow);
    boot.safe = false;
    for (int state = ESP_OTA_IMG_NEW; state <= ESP_OTA_IMG_ABORTED; state++) {
        ota.running.state = state;
        assert((command("BEGIN 12345678 %zu %s", fixture_size, digest_hex) == ESP_OK) == (state == ESP_OTA_IMG_VALID));
        if (file >= 0) {
            assert(command("ABORT 12345678") == ESP_OK);
        }
    }
    ota.running.state = ESP_OTA_IMG_VALID;
    for (int phase = ROUNDIE_OTA_RECEIVING; phase <= ROUNDIE_OTA_ACTIVATION_PENDING; phase++) {
        ota.phase = phase;
        assert(command("RUN %s", digest_hex) == ESP_ERR_INVALID_STATE && !allow);
    }
    ota.phase = ROUNDIE_OTA_IDLE;
    mounted = false;
    assert(command("RUN %s", digest_hex) == ESP_ERR_INVALID_STATE);
    mounted = true;
    ota.boot_passed = false;
    assert(command("RUN %s", digest_hex) == ESP_ERR_INVALID_STATE);
    ota.boot_passed = true;
    app_busy = true;
    assert(command("BEGIN 12345678 %zu %s", fixture_size, digest_hex) == ESP_ERR_INVALID_STATE);
    app_busy = false;
    package_busy = true;
    assert(roundie_app_runtime_busy());
    assert(command("BEGIN 12345678 %zu %s", fixture_size, digest_hex) == ESP_ERR_INVALID_STATE);
    assert(command("RUN %s", digest_hex) == ESP_ERR_INVALID_STATE && !allow);
    package_busy = false;
    assert(command("RUN ../hello.elf") != ESP_OK);
    assert(command("RESTART") != ESP_OK);
    assert(command("STATUS suffix") != ESP_OK);
    assert(command("BEGIN 12345678 65537 %s", digest_hex) == ESP_ERR_INVALID_SIZE);
    send_file(fixture, digest_hex);
    assert(command("END 12345678") == ESP_OK);
    assert_staged_original();
    assert(command("RUN %s", digest_hex) == ESP_OK && submissions == 1);
    assert(command("RUN %064d", 0) != ESP_OK && submissions == 1);
    assert(command("BEGIN 12345678 %zu %s", fixture_size, digest_hex) == ESP_OK);
    clock_us += 10000001;
    roundie_elf_poll(clock_us);
    assert(!roundie_app_runtime_busy() && access(TEMPORARY_ELF, F_OK) != 0);
    assert_staged_original();
    assert(command("BEGIN 12345678 %zu %s", fixture_size, digest_hex) == ESP_OK);
    boot.safe = true;
    roundie_elf_poll(clock_us);
    assert(!roundie_app_runtime_busy() && !allow);
    boot.safe = false;
    assert_staged_original();
    /* Deliberately corrupt bytes with original hash: integrity failure. */
    uint8_t invalid[ROUNDIE_ELF_MAX_FILE];
    memcpy(invalid, fixture, fixture_size);
    invalid[0] = 0;
    send_file(invalid, digest_hex);
    assert(command("END 12345678") != ESP_OK);
    assert(strstr(last_error, "integrity") != NULL);
    assert_staged_original();
    /* A correct transfer hash cannot bypass preflight. */
    char bad_hash[65];
    digest(invalid, fixture_size, bad_hash);
    send_file(invalid, bad_hash);
    assert(command("END 12345678") != ESP_OK && strcmp(last_error, "magic") == 0);
    assert_staged_original();
    assert(submissions == 1 && heap_live == 0);
#ifdef ROUNDIE_IO_READ
    /* Substituted VFS: EOF inside a 4 KiB chunk of read_candidate() (END reads
     * the temporary file, RUN the staged one) is refused; the staged ELF is
     * never replaced and nothing is submitted. Needs an ELF over one chunk. */
    assert(argc == 3);
    static uint8_t original[ROUNDIE_ELF_MAX_FILE];
    static uint8_t large[ROUNDIE_ELF_MAX_FILE];
    size_t original_size = fixture_size;
    memcpy(original, fixture, original_size);
    source = fopen(argv[2], "rb");
    assert(source != NULL);
    size_t large_size = fread(large, 1, sizeof(large), source);
    assert(fclose(source) == 0 && large_size > ROUNDIE_IO_CHUNK + 512);
    char large_hex[65];
    digest(large, large_size, large_hex);
    const size_t eof_at[] = {20, 2000, ROUNDIE_IO_CHUNK + 100};
    for (unsigned i = 0; i < sizeof(eof_at) / sizeof(eof_at[0]); i++) {
        fixture_size = large_size;
        send_file(large, large_hex);
        fake_vfs_eof_after(eof_at[i]);
        assert(command("END 12345678") != ESP_OK);
        fake_vfs_disarm();
        assert(strcmp(last_error, "candidate-read-or-allocation") == 0);
        assert(access(TEMPORARY_ELF, F_OK) != 0);
        fixture_size = original_size;
        assert_staged_original();
    }
    fixture_size = large_size;
    memcpy(fixture, large, large_size);
    send_file(large, large_hex);
    assert(command("END 12345678") == ESP_OK);
    assert_staged_original();
    unsigned before = submissions;
    for (unsigned i = 0; i < sizeof(eof_at) / sizeof(eof_at[0]); i++) {
        fake_vfs_eof_after(eof_at[i]);
        assert(command("RUN %s", large_hex) != ESP_OK);
        fake_vfs_disarm();
        assert(strcmp(last_error, "candidate-read-or-allocation") == 0);
        assert(submissions == before && heap_live == 0);
        assert_staged_original();
    }
    assert(command("RUN %s", large_hex) == ESP_OK && submissions == before + 1 && heap_live == 0);
#endif
    printf("ELF production RE1 staging/policy PASS: Safe Mode, OTA, hash, bounds, timeout, preservation\n");
}
