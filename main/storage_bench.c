#include "storage_bench.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"
#include "app_runtime.h"
#include "storage_diagnostics.h"

#define BENCH_DIRECTORY "/storage/dev"
#define BENCH_CHUNK 4096U
/* Feed the task watchdog every 64 KiB; the worker is subscribed to it. */
#define BENCH_CHUNKS_PER_FEED 16U
/* Space kept free beyond the file for LittleFS metadata and other writers. */
#define BENCH_RESERVE (128U * 1024U)
/* Fixed seed of the test pattern; tools/hardware_validation/run_e0.py
 * computes the same bytes to check the reported SHA-256. */
#define BENCH_SEED 0x9E3779B9U

typedef enum {
    BENCH_PREP,
    BENCH_READ,
    BENCH_DEL,
} bench_operation_t;

typedef struct {
    uint32_t bytes;
    int64_t write_us;
    int64_t read_us;
    int64_t sha_us;
    int64_t total_us;
    int64_t chunk_max_us;
    size_t total;
    size_t used;
    bool usage;
    char digest[65];
} bench_result_t;

/* System worker only: the sole owner of this scratch. */
static uint8_t chunk[BENCH_CHUNK];

static bool parse(const char *operation, bench_operation_t *kind, uint32_t *kib)
{
    const char *size;
    if (strncmp(operation, "PREP ", 5) == 0) {
        *kind = BENCH_PREP;
        size = operation + 5;
    } else if (strncmp(operation, "READ ", 5) == 0) {
        *kind = BENCH_READ;
        size = operation + 5;
    } else if (strncmp(operation, "DEL ", 4) == 0) {
        *kind = BENCH_DEL;
        size = operation + 4;
    } else {
        return false;
    }
    if (strcmp(size, "64") == 0) {
        *kib = 64U;
    } else if (strcmp(size, "512") == 0) {
        *kib = 512U;
    } else if (strcmp(size, "1024") == 0) {
        *kib = 1024U;
    } else {
        return false;
    }
    return true;
}

static void bench_path(char *path, size_t capacity, uint32_t kib)
{
    snprintf(path, capacity, BENCH_DIRECTORY "/e0-%" PRIu32 ".bin", kib);
}

/* xorshift32, little-endian words. The state carries across chunks. */
static void fill_pattern(uint8_t *bytes, size_t size, uint32_t *state)
{
    for (size_t i = 0; i < size; i += 4U) {
        uint32_t x = *state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *state = x;
        bytes[i] = (uint8_t)x;
        bytes[i + 1U] = (uint8_t)(x >> 8);
        bytes[i + 2U] = (uint8_t)(x >> 16);
        bytes[i + 3U] = (uint8_t)(x >> 24);
    }
}

static void feed_watchdog(uint32_t chunk_index)
{
    if (chunk_index % BENCH_CHUNKS_PER_FEED == 0U) {
        esp_task_wdt_reset();
    }
}

static esp_err_t prepare(const char *path, uint32_t bytes, bench_result_t *result)
{
    if (roundie_app_runtime_busy()) {
        /* A Flash write stalls the cache that native code runs from. */
        return ESP_ERR_INVALID_STATE;
    }
    if (!result->usage || result->total < result->used ||
        result->total - result->used < (size_t)bytes + BENCH_RESERVE) {
        return ESP_ERR_NO_MEM;
    }
    if (mkdir(BENCH_DIRECTORY, 0700) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    int64_t started = esp_timer_get_time();
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) {
        return ESP_FAIL;
    }
    esp_err_t error = ESP_OK;
    uint32_t state = BENCH_SEED;
    for (uint32_t offset = 0; offset < bytes && error == ESP_OK; offset += BENCH_CHUNK) {
        fill_pattern(chunk, BENCH_CHUNK, &state);
        if (write(descriptor, chunk, BENCH_CHUNK) != (ssize_t)BENCH_CHUNK) {
            error = ESP_FAIL;
        }
        feed_watchdog(offset / BENCH_CHUNK);
    }
    if (error == ESP_OK && fsync(descriptor) != 0) {
        error = ESP_FAIL;
    }
    if (close(descriptor) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        unlink(path);
    }
    result->write_us = esp_timer_get_time() - started;
    return error;
}

static esp_err_t measure(const char *path, uint32_t bytes, bench_result_t *result)
{
    if (!roundie_storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t started = esp_timer_get_time();
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    struct stat info;
    if (fstat(descriptor, &info) != 0 || info.st_size != (off_t)bytes) {
        close(descriptor);
        return ESP_ERR_INVALID_SIZE;
    }
    psa_hash_operation_t hash = psa_hash_operation_init();
    esp_err_t error = ESP_OK;
    int64_t sha_started = esp_timer_get_time();
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        error = ESP_FAIL;
    }
    result->sha_us += esp_timer_get_time() - sha_started;
    for (uint32_t offset = 0; offset < bytes && error == ESP_OK; offset += BENCH_CHUNK) {
        int64_t chunk_started = esp_timer_get_time();
        ssize_t count = read(descriptor, chunk, BENCH_CHUNK);
        int64_t read_done = esp_timer_get_time();
        if (count != (ssize_t)BENCH_CHUNK) {
            error = ESP_FAIL;
            break;
        }
        if (psa_hash_update(&hash, chunk, BENCH_CHUNK) != PSA_SUCCESS) {
            error = ESP_FAIL;
        }
        int64_t hash_done = esp_timer_get_time();
        result->read_us += read_done - chunk_started;
        result->sha_us += hash_done - read_done;
        if (hash_done - chunk_started > result->chunk_max_us) {
            result->chunk_max_us = hash_done - chunk_started;
        }
        feed_watchdog(offset / BENCH_CHUNK);
    }
    uint8_t digest[32];
    size_t digest_size = 0;
    sha_started = esp_timer_get_time();
    if (error == ESP_OK &&
        (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS || digest_size != 32U)) {
        error = ESP_FAIL;
    }
    result->sha_us += esp_timer_get_time() - sha_started;
    psa_hash_abort(&hash);
    if (close(descriptor) != 0 && error == ESP_OK) {
        error = ESP_FAIL;
    }
    result->total_us = esp_timer_get_time() - started;
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 32U; i++) {
            snprintf(result->digest + i * 2U, 3, "%02x", digest[i]);
        }
    }
    return error;
}

bool roundie_storage_bench_command(const char *operation, const char *id, int parsed)
{
    if (strncmp(operation, "FSBENCH ", 8) != 0) {
        return false;
    }
    bench_operation_t kind = BENCH_READ;
    uint32_t kib = 0;
    esp_err_t error = parsed;
    if (error == ESP_OK && !parse(operation + 8, &kind, &kib)) {
        error = ESP_ERR_INVALID_ARG;
    }
    static bench_result_t result;
    memset(&result, 0, sizeof(result));
    result.bytes = kib * 1024U;
    char path[48];
    bench_path(path, sizeof(path), kib);
    if (error == ESP_OK && !roundie_app_runtime_allowed()) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        result.usage = roundie_storage_usage(&result.total, &result.used);
        if (kind == BENCH_PREP) {
            error = prepare(path, result.bytes, &result);
        } else if (kind == BENCH_READ) {
            error = measure(path, result.bytes, &result);
        } else if (roundie_app_runtime_busy()) {
            error = ESP_ERR_INVALID_STATE;
        } else if (unlink(path) != 0) {
            error = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
        if (kind != BENCH_READ) {
            result.usage = roundie_storage_usage(&result.total, &result.used);
        }
    }
    static const char *const names[] = {"PREP", "READ", "DEL"};
    static char response[400];
    int length = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"fsbench\":\"%s\",\"kib\":%" PRIu32 ",\"bytes\":%" PRIu32
        ",\"write_us\":%" PRId64 ",\"read_us\":%" PRId64 ",\"sha_us\":%" PRId64 ",\"total_us\":%" PRId64
        ",\"chunk\":%u,\"chunk_max_us\":%" PRId64 ",\"capacity\":[%u,%zu,%zu],\"sha256\":\"%s\"}\n",
        id, (int)error, names[kind], kib, result.bytes, result.write_us, result.read_us, result.sha_us,
        result.total_us, BENCH_CHUNK, result.chunk_max_us, result.usage ? 1U : 0U, result.total, result.used,
        result.digest);
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}
#else
bool roundie_storage_bench_command(const char *operation, const char *id, int parsed)
{
    (void)operation;
    (void)id;
    (void)parsed;
    return false;
}
#endif
