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
#include "esp_heap_caps.h"
#include "esp_private/flash_mmap.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "app_runtime.h"
#include "stall_probe.h"
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

/* ---- XREAD/XWRITE: E0 follow-up stall experiments (docs/baseline/e0-1.85b.md §10).
 * "X<READ|WRITE> <kib> <chunk> <yield_ticks> <W|C0|C1>": the same file and
 * pattern as PREP/READ, in chunks of <chunk> bytes, with vTaskDelay(yield)
 * after each chunk, run in the system worker (W) or in a helper task of the
 * worker's priority pinned to core 0 or 1 (C0/C1) while the worker waits. */
#define X_CHUNK_MIN 512U
#define X_CHUNK_MAX (32U * 1024U)
#define X_YIELD_MAX 10U
#define X_FEED_BYTES (64U * 1024U)
#define X_HELPER_STACK 8192U
#define X_HELPER_TIMEOUT_US (60LL * 1000000LL)

typedef struct {
    bool write;
    uint32_t bytes;
    uint32_t chunk;
    uint32_t yield_ticks;
    int core; /* -1: in the system worker */
    bool feed;
    uint8_t *buffer;
    char path[48];
    TaskHandle_t waiter;
    esp_err_t error;
    int64_t io_us;
    int64_t sha_us;
    int64_t total_us;
    int64_t chunk_max_us;
    uint32_t on_core[2];
    uint32_t migrations;
    char digest[65];
} x_job_t;

static x_job_t x_job;
static volatile bool x_helper_running;

static bool parse_x(const char *text, x_job_t *job, uint32_t *kib)
{
    if (strncmp(text, "XREAD ", 6) == 0) {
        job->write = false;
        text += 6;
    } else if (strncmp(text, "XWRITE ", 7) == 0) {
        job->write = true;
        text += 7;
    } else {
        return false;
    }
    unsigned size = 0;
    unsigned chunk_bytes = 0;
    unsigned yield_ticks = 0;
    char where[3] = {0};
    int consumed = 0;
    if (sscanf(text, "%u %u %u %2s%n", &size, &chunk_bytes, &yield_ticks, where, &consumed) != 4 ||
        text[consumed] != '\0') {
        return false;
    }
    if (size != 64U && size != 512U && size != 1024U) {
        return false;
    }
    if (chunk_bytes < X_CHUNK_MIN || chunk_bytes > X_CHUNK_MAX || (chunk_bytes & (chunk_bytes - 1U)) != 0U ||
        yield_ticks > X_YIELD_MAX) {
        return false;
    }
    if (strcmp(where, "W") == 0) {
        job->core = -1;
    } else if (strcmp(where, "C0") == 0) {
        job->core = 0;
    } else if (strcmp(where, "C1") == 0) {
        job->core = 1;
    } else {
        return false;
    }
    *kib = size;
    job->chunk = chunk_bytes;
    job->yield_ticks = yield_ticks;
    return true;
}

static void x_execute(x_job_t *job)
{
    int64_t started = esp_timer_get_time();
    int previous_core = -1;
    int descriptor = open(job->path, job->write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0600);
    if (descriptor < 0) {
        job->error = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        return;
    }
    struct stat info;
    if (!job->write && (fstat(descriptor, &info) != 0 || info.st_size != (off_t)job->bytes)) {
        close(descriptor);
        job->error = ESP_ERR_INVALID_SIZE;
        return;
    }
    psa_hash_operation_t hash = psa_hash_operation_init();
    if (!job->write && psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        job->error = ESP_FAIL;
    }
    uint32_t state = BENCH_SEED;
    for (uint32_t offset = 0; offset < job->bytes && job->error == ESP_OK; offset += job->chunk) {
        int core = xPortGetCoreID();
        job->on_core[core]++;
        if (previous_core >= 0 && core != previous_core) {
            job->migrations++;
        }
        previous_core = core;
        int64_t chunk_started = esp_timer_get_time();
        if (job->write) {
            fill_pattern(job->buffer, job->chunk, &state);
            int64_t io_started = esp_timer_get_time();
            if (write(descriptor, job->buffer, job->chunk) != (ssize_t)job->chunk) {
                job->error = ESP_FAIL;
            }
            job->io_us += esp_timer_get_time() - io_started;
        } else {
            ssize_t count = read(descriptor, job->buffer, job->chunk);
            int64_t read_done = esp_timer_get_time();
            job->io_us += read_done - chunk_started;
            if (count != (ssize_t)job->chunk || psa_hash_update(&hash, job->buffer, job->chunk) != PSA_SUCCESS) {
                job->error = ESP_FAIL;
            }
            job->sha_us += esp_timer_get_time() - read_done;
        }
        int64_t chunk_us = esp_timer_get_time() - chunk_started;
        if (chunk_us > job->chunk_max_us) {
            job->chunk_max_us = chunk_us;
        }
        if (job->feed && (offset + job->chunk) % X_FEED_BYTES == 0U) {
            esp_task_wdt_reset();
        }
        if (job->yield_ticks != 0U) {
            vTaskDelay(job->yield_ticks);
        }
    }
    if (job->write && job->error == ESP_OK) {
        int64_t sync_started = esp_timer_get_time();
        if (fsync(descriptor) != 0) {
            job->error = ESP_FAIL;
        }
        job->io_us += esp_timer_get_time() - sync_started;
    }
    if (close(descriptor) != 0 && job->error == ESP_OK) {
        job->error = ESP_FAIL;
    }
    if (job->write && job->error != ESP_OK) {
        unlink(job->path);
    }
    if (!job->write) {
        uint8_t digest[32];
        size_t digest_size = 0;
        if (job->error == ESP_OK &&
            (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS || digest_size != 32U)) {
            job->error = ESP_FAIL;
        }
        psa_hash_abort(&hash);
        if (job->error == ESP_OK) {
            for (unsigned i = 0; i < 32U; i++) {
                snprintf(job->digest + i * 2U, 3, "%02x", digest[i]);
            }
        }
    }
    job->total_us = esp_timer_get_time() - started;
}

static void x_helper(void *argument)
{
    x_job_t *job = argument;
    x_execute(job);
    TaskHandle_t waiter = job->waiter;
    x_helper_running = false;
    xTaskNotifyGive(waiter);
    vTaskDelete(NULL);
}

/* Runs the job where requested; the worker keeps feeding its watchdog. */
static esp_err_t x_run(x_job_t *job)
{
    if (job->core < 0) {
        job->feed = true;
        x_execute(job);
        return job->error;
    }
    job->feed = false;
    job->waiter = xTaskGetCurrentTaskHandle();
    x_helper_running = true;
    if (xTaskCreatePinnedToCore(x_helper, "fsbench_x", X_HELPER_STACK, job, uxTaskPriorityGet(NULL), NULL,
                                job->core) != pdPASS) {
        x_helper_running = false;
        return ESP_ERR_NO_MEM;
    }
    int64_t deadline = esp_timer_get_time() + X_HELPER_TIMEOUT_US;
    while (x_helper_running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        esp_task_wdt_reset();
        if (x_helper_running && esp_timer_get_time() > deadline) {
            /* The helper still owns the job; later X commands are refused. */
            return ESP_ERR_TIMEOUT;
        }
    }
    return job->error;
}

static void x_command(const char *operation, const char *id, int parsed)
{
    uint32_t kib = 0;
    esp_err_t error = parsed;
    roundie_stall_report_t stall = {0};
    bool armed = false;
    if (x_helper_running) {
        error = ESP_ERR_INVALID_STATE;
    }
    x_job_t *job = &x_job;
    if (error == ESP_OK) {
        memset(job, 0, sizeof(*job));
        if (!parse_x(operation, job, &kib)) {
            error = ESP_ERR_INVALID_ARG;
        }
    }
    job->bytes = kib * 1024U;
    bench_path(job->path, sizeof(job->path), kib);
    size_t total = 0;
    size_t used = 0;
    bool usage = false;
    if (error == ESP_OK && (!roundie_app_runtime_allowed() || !roundie_storage_available())) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (error == ESP_OK) {
        usage = roundie_storage_usage(&total, &used);
        if (job->write && roundie_app_runtime_busy()) {
            /* As PREP: no Flash write while native code runs. */
            error = ESP_ERR_INVALID_STATE;
        } else if (job->write && (!usage || total < used || total - used < (size_t)job->bytes + BENCH_RESERVE)) {
            error = ESP_ERR_NO_MEM;
        } else if (job->write && mkdir(BENCH_DIRECTORY, 0700) != 0 && errno != EEXIST) {
            error = ESP_FAIL;
        }
    }
    if (error == ESP_OK) {
        job->buffer = heap_caps_malloc(job->chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (job->buffer == NULL) {
            error = ESP_ERR_NO_MEM;
        }
    }
    bool mmap_remain = flash_mmap_remain();
    if (error == ESP_OK) {
        armed = roundie_stall_arm();
        error = x_run(job);
        roundie_stall_disarm(&stall);
        if (error != ESP_ERR_TIMEOUT) {
            heap_caps_free(job->buffer);
            job->buffer = NULL;
        }
    }
    if (job->write) {
        usage = roundie_storage_usage(&total, &used);
    }
    static char response[900];
    int length = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"fsbench\":\"%s\",\"kib\":%" PRIu32 ",\"chunk\":%" PRIu32
        ",\"yield\":%" PRIu32 ",\"core\":%d,\"io_us\":%" PRId64 ",\"sha_us\":%" PRId64 ",\"total_us\":%" PRId64
        ",\"chunk_max_us\":%" PRId64 ",\"on_core\":[%" PRIu32 ",%" PRIu32 "],\"migrations\":%" PRIu32
        ",\"mmap_remain\":%u,\"probes_armed\":%u,\"gui\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"probes\":[",
        id, (int)error, job->write ? "XWRITE" : "XREAD", kib, job->chunk, job->yield_ticks, job->core,
        job->io_us, job->sha_us, job->total_us, job->chunk_max_us, job->on_core[0], job->on_core[1],
        job->migrations, mmap_remain ? 1U : 0U, armed ? 1U : 0U, stall.gui_iterations, stall.gui_gap_max_us,
        stall.gui_late_max_us);
    for (unsigned i = 0; i < ROUNDIE_STALL_PROBES && length > 0 && (size_t)length < sizeof(response); i++) {
        length += snprintf(response + length, sizeof(response) - (size_t)length, "%s[%u,%u,%" PRIu32 ",%" PRIu32 "]",
            i == 0U ? "" : ",", stall.probe[i].core, stall.probe[i].priority, stall.probe[i].wakes,
            stall.probe[i].gap_max_us);
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        length += snprintf(response + length, sizeof(response) - (size_t)length,
            "],\"capacity\":[%u,%zu,%zu],\"sha256\":\"%s\"}\n", usage ? 1U : 0U, total, used, job->digest);
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
}

bool roundie_storage_bench_command(const char *operation, const char *id, int parsed)
{
    if (strncmp(operation, "FSBENCH ", 8) != 0) {
        return false;
    }
    if (operation[8] == 'X') {
        x_command(operation + 8, id, parsed);
        return true;
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
