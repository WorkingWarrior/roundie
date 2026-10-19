#include "elf_diagnostics.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_ELF
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"
#include "roundie_elf.h"
#include "roundie_ota_protocol.h"
#include "roundie_boot_guard.h"
#include "storage_diagnostics.h"

#ifndef DEVELOPMENT_DIRECTORY
#define DEVELOPMENT_DIRECTORY "/storage/dev"
#endif
#define TEMPORARY_ELF DEVELOPMENT_DIRECTORY "/.hello.part"
#define STAGED_ELF DEVELOPMENT_DIRECTORY "/hello.elf"

/* The existing system worker is the sole staging/VFS/USB owner. No open file
 * or worker accesses relocated code. StorageManager retains mount ownership. */
static int file = -1;
static uint32_t session;
static size_t received;
static size_t total;
static uint8_t expected[32];
static int64_t deadline;
static char last_error[ROUNDIE_ELF_ERROR_SIZE];
static bool permitted;

static bool policy(void)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return !boot.safe && boot.stage == ROUNDIE_BOOT_STABLE &&
        ota.initialized && ota.boot_checked && ota.boot_passed &&
        ota.running.state == ESP_OTA_IMG_VALID && ota.phase == ROUNDIE_OTA_IDLE &&
        strcmp(ota.running.label, ota.boot) == 0 && roundie_storage_available();
}

static void abort_transfer(void)
{
    if (file >= 0) {
        close(file);
        file = -1;
        unlink(TEMPORARY_ELF);
    }
    session = 0;
    received = 0;
    total = 0;
}

bool roundie_elf_development_busy(void)
{
    return file >= 0 || roundie_elf_busy();
}

void roundie_elf_poll(int64_t now_us)
{
    permitted = policy();
    roundie_elf_set_allowed(permitted);
    if (file >= 0 && (!permitted || now_us >= deadline)) {
        snprintf(last_error, sizeof(last_error), "transfer-cancelled-or-expired");
        abort_transfer();
    }
}

static uint8_t *read_candidate(const char *path, size_t *size, const uint8_t digest[32])
{
    struct stat info;
    int descriptor = open(path, O_RDONLY);
    uint8_t *bytes = NULL;
    if (descriptor < 0) {
        goto failed;
    }
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 52 ||
        info.st_size > ROUNDIE_ELF_MAX_FILE) {
        goto failed;
    }
    *size = (size_t)info.st_size;
    bytes = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bytes == NULL || read(descriptor, bytes, *size) != (ssize_t)*size) {
        goto failed;
    }
    int closed = close(descriptor);
    descriptor = -1;
    if (closed != 0) {
        goto failed;
    }
    uint8_t actual[32];
    size_t actual_size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, bytes, *size, actual, sizeof(actual), &actual_size) != PSA_SUCCESS ||
        actual_size != 32 || memcmp(actual, digest, 32) != 0) {
        snprintf(last_error, sizeof(last_error), "integrity-SHA256");
        goto failed;
    }
    roundie_elf_info_t candidate;
    if (!roundie_elf_validate(bytes, *size, &candidate, last_error)) {
        goto failed;
    }
    return bytes;
failed:
    if (descriptor >= 0) {
        close(descriptor);
    }
    heap_caps_free(bytes);
    if (last_error[0] == '\0') {
        snprintf(last_error, sizeof(last_error), "candidate-read-or-allocation");
    }
    return NULL;
}

static esp_err_t operate(const roundie_ota_command_t *command, bool start)
{
    if (command->operation == ROUNDIE_OTA_STATUS) {
        return ESP_OK;
    }
    if (command->operation == ROUNDIE_OTA_ABORT) {
        if (file < 0 || session != command->session) {
            return ESP_ERR_INVALID_STATE;
        }
        abort_transfer();
        return ESP_OK;
    }
    if (!permitted || roundie_elf_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (command->operation == ROUNDIE_OTA_BEGIN) {
        if (file >= 0 || command->size < 52 || command->size > ROUNDIE_ELF_MAX_FILE) {
            return ESP_ERR_INVALID_SIZE;
        }
        last_error[0] = '\0';
        if (mkdir(DEVELOPMENT_DIRECTORY, 0700) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
        /* Explicit BEGIN cleans only our interrupted temporary file. */
        if (unlink(TEMPORARY_ELF) != 0 && errno != ENOENT) {
            return ESP_FAIL;
        }
        file = open(TEMPORARY_ELF, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file < 0) {
            return ESP_FAIL;
        }
        session = command->session;
        total = command->size;
        received = 0;
        memcpy(expected, command->digest, 32);
        deadline = esp_timer_get_time() + 10000000;
        return ESP_OK;
    }
    if (command->operation == ROUNDIE_OTA_ACTIVATE) { /* RE1 spelling: RUN */
        if (file >= 0) {
            return ESP_ERR_INVALID_STATE;
        }
        last_error[0] = '\0';
        size_t size = 0;
        uint8_t *bytes = read_candidate(STAGED_ELF, &size, command->digest);
        if (bytes == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t error = start ? roundie_elf_start(bytes, size) : roundie_elf_submit(bytes, size);
        if (error != ESP_OK) {
            heap_caps_free(bytes);
        }
        return error;
    }
    if (file < 0 || command->session != session) {
        return ESP_ERR_INVALID_STATE;
    }
    if (command->operation == ROUNDIE_OTA_DATA) {
        if (command->offset != received || command->size == 0 || command->size > total - received) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (write(file, command->data, command->size) != (ssize_t)command->size) {
            abort_transfer();
            return ESP_FAIL;
        }
        received += command->size;
        deadline = esp_timer_get_time() + 10000000;
        return ESP_OK;
    }
    if (command->operation == ROUNDIE_OTA_END) {
        if (received != total) {
            return ESP_ERR_INVALID_SIZE;
        }
        int synced = fsync(file);
        int closed = close(file);
        file = -1;
        size_t size = 0;
        uint8_t *bytes = synced == 0 && closed == 0 ? read_candidate(TEMPORARY_ELF, &size, expected) : NULL;
        esp_err_t error = ESP_ERR_INVALID_ARG;
        if (bytes != NULL && size == total && rename(TEMPORARY_ELF, STAGED_ELF) == 0) {
            error = ESP_OK;
        }
        heap_caps_free(bytes);
        if (error != ESP_OK) {
            unlink(TEMPORARY_ELF);
        }
        session = 0;
        received = 0;
        total = 0;
        return error;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool decimal(const char **text, uint32_t *value)
{
    *value = 0;
    const char *p = *text;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (*value > (UINT32_MAX - digit) / 10) {
            return false;
        }
        *value = *value * 10 + digit;
        p++;
    }
    *text = p;
    return true;
}

static esp_err_t runtime_command(const char *operation)
{
    if (strcmp(operation, "RESOURCES") == 0) {
        return ESP_OK;
    }
    static const char *const names[] = {"SUSPEND ", "RESUME ", "STOP ", "UNLOAD ", "EVENT "};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t length = strlen(names[i]);
        if (strncmp(operation, names[i], length) != 0) {
            continue;
        }
        const char *argument = operation + length;
        uint32_t generation;
        if (!decimal(&argument, &generation) || generation == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (i == 4) {
            uint32_t value;
            if (*argument != ' ') {
                return ESP_ERR_INVALID_ARG;
            }
            argument++;
            if (!decimal(&argument, &value) || *argument != '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            return roundie_elf_post(generation, value);
        }
        if (*argument != '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        return roundie_elf_control(generation, (roundie_elf_control_t)(i + 1));
    }
    return ESP_ERR_INVALID_ARG;
}

void roundie_elf_command(char *line, bool discard)
{
    roundie_elf_poll(esp_timer_get_time());
    roundie_ota_command_t command;
    /* Save only the bounded runtime operation before the shared transfer parser
     * tokenizes it. A second STATUS parse validates its exact frame/ID. */
    char runtime[96] = {0};
    size_t length_in = strlen(line);
    bool start = length_in >= 20 && strncmp(line + 14, "START ", 6) == 0;
    if (length_in >= 14 && length_in - 14 < sizeof(runtime)) {
        memcpy(runtime, line + 14, length_in - 14 + 1);
    }
    esp_err_t error = roundie_transfer_parse(line, discard, "!RE1 ", start ? "START" : "RUN", &command);
    if (error == ESP_OK) {
        error = operate(&command, start);
    } else if (runtime[0] != '\0' && length_in >= 14) {
        char frame[21];
        memcpy(frame, line, 14);
        memcpy(frame + 14, "STATUS", 7);
        if (roundie_transfer_parse(frame, discard, "!RE1 ", "RUN", &command) == ESP_OK) {
            error = runtime_command(runtime);
        }
    }
    roundie_elf_status_t current;
    roundie_elf_status(&current);
    char safe_error[ROUNDIE_ELF_ERROR_SIZE];
    const char *source = last_error[0] != '\0' ? last_error : current.error;
    for (size_t i = 0; i < sizeof(safe_error); i++) {
        char ch = source[i];
        safe_error[i] = ch == '\0' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || ch == '_' || ch == '-' ? ch : '_';
        if (ch == '\0') {
            break;
        }
    }
    safe_error[sizeof(safe_error) - 1] = '\0';
    /* Five fixed-width counters per snapshot: internal free/largest, PSRAM
     * free, executable free/largest. Compact arrays keep one bounded reply. */
#define MF "[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]"
#define MV(m) (m).internal_free, (m).internal_largest, (m).psram_free, (m).exec_free, (m).exec_largest
    char response[2048];
    int length = snprintf(response, sizeof(response),
        "\n!RE1 %s %d {\"allowed\":%u,\"busy\":%u,\"session\":\"%08" PRIx32 "\",\"received\":%zu,\"total\":%zu,"
        "\"state\":%u,\"instance\":%" PRIu32 ",\"completed\":%" PRIu32 ",\"abi\":%" PRIu32 ",\"loader\":\"1.3.5\","
        "\"code\":%" PRIu32 ",\"data\":%" PRIu32 ",\"alloc\":%" PRIu32 ",\"alloc_peak\":%" PRIu32 ",\"calls\":%" PRIu32 ","
        "\"load_us\":%" PRIu32 ",\"execute_us\":%" PRIu32 ",\"unload_us\":%" PRIu32 ","
        "\"loader_result\":%d,\"app_result\":%" PRId32 ",\"executed\":%u,\"over_budget\":%u,\"error\":\"%s\","
        "\"before\":" MF ",\"relocated\":" MF ",\"during\":" MF ",\"after\":" MF ","
        "\"callbacks\":%" PRIu32 ",\"callback_max_us\":%" PRIu32 ",\"init_us\":%" PRIu32 ","
        "\"suspend_us\":%" PRIu32 ",\"resume_us\":%" PRIu32 ",\"shutdown_us\":%" PRIu32 ","
        "\"events\":%" PRIu32 ",\"ticks\":%" PRIu32 ",\"users\":%" PRIu32 ",\"value\":%" PRIu32 ","
        "\"latency_max_us\":%" PRIu32 ",\"queue\":%" PRIu32 ",\"dropped\":%" PRIu32 ","
        "\"coalesced\":%" PRIu32 ",\"rejected\":%" PRIu32 ","
        "\"resources\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],"
        "\"timer\":%" PRIu32 ",\"subscriptions\":%" PRIu32 ","
        "\"control\":[%" PRIu32 ",%" PRIu32 ",%" PRId32 "]}\n",
        command.id, (int)error, permitted, roundie_elf_development_busy(), session, received, total,
        (unsigned)current.state, current.instance, current.completed, current.abi_version, current.code_bytes, current.data_bytes,
        current.alloc_bytes, current.alloc_peak, current.api_calls, current.load_us, current.execute_us,
        current.unload_us, current.loader_result, current.app_result, current.executed, current.over_budget,
        safe_error, MV(current.before), MV(current.relocated), MV(current.during), MV(current.after),
        current.callbacks, current.callback_max_us, current.init_us, current.suspend_us, current.resume_us,
        current.shutdown_us, current.events_delivered, current.timers_delivered, current.users_delivered,
        current.last_value, current.latency_max_us, current.queue_depth, current.events_dropped,
        current.events_coalesced, current.events_rejected, current.resources_acquired,
        current.resources_released, current.resources_reclaimed, current.resources_outstanding,
        current.timer_handle, current.subscriptions, current.control_submitted,
        current.control_completed, current.control_result);
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
}
#endif
