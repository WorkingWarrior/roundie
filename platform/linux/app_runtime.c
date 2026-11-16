/* Application runtime of the Linux desktop (docs/native-apps.md, N4-N7).
 *
 * The shared manager (components/roundie_elf/manager.c) runs linux-x64.rep1
 * applications through the desktop loader and executor
 * (components/roundie_elf/linux). Native applications are trusted developer
 * code in the desktop process and run only when the desktop was started with
 * --native-apps; otherwise every launch is refused as before. This file
 * implements the package manager's integration seams
 * (components/roundie_packages/platform.h) and the application storage flow of
 * main/app_runtime.c. */

#define _GNU_SOURCE
#include "app_runtime.h"
#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <ucontext.h>
#include <unistd.h>
#include "esp_log.h"
#include "platform.h"
#include "rep/rep_storage_v1.h"
#include "roundie_elf.h"
#include "roundie_packages.h"
#include "roundie_platform.h"
#include "vhw.h"

/* An executor call running this long is reported as an application that does not return. */
#define STALL_REPORT_US (5 * 1000 * 1000)
/* In the data directory while an application is loaded: BootGuard's record of
 * the loaded package (main/app_runtime.c), read after an abnormal exit. */
#define LOADED_MARKER "app-loaded"
#define FAULT_REPORT_BYTES 512U

static const int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
static struct sigaction previous_actions[sizeof(fault_signals) / sizeof(fault_signals[0])];

static atomic_bool native_apps;
static _Atomic(roundie_elf_clock_source_t) clock_source;

/* Worker only: package whose application storage the loaded or last instance holds. */
static char storage_owner[ROUNDIE_PACKAGE_ID];
static uint8_t storage_buffer[REP_STORAGE_CAPACITY];
static int64_t stall_reported;
/* Worker writes, the fault handler reads: the package the marker names. */
static char loaded_id[ROUNDIE_PACKAGE_ID];
static bool marked;

/* Async-signal-safe text building for the fault report. */
static size_t append_text(char *buffer, size_t used, const char *text)
{
    while (*text != '\0' && used + 1 < FAULT_REPORT_BYTES) {
        buffer[used] = *text;
        used++;
        text++;
    }
    buffer[used] = '\0';
    return used;
}

static size_t append_number(char *buffer, size_t used, uintptr_t value, unsigned radix)
{
    char digits[24];
    size_t count = 0;
    do {
        digits[count] = "0123456789abcdef"[value % radix];
        count++;
        value /= radix;
    } while (value != 0 && count < sizeof(digits));
    if (radix == 16) {
        used = append_text(buffer, used, "0x");
    }
    while (count > 0 && used + 1 < FAULT_REPORT_BYTES) {
        count--;
        buffer[used] = digits[count];
        used++;
    }
    buffer[used] = '\0';
    return used;
}

static const char *signal_name(int signal_number)
{
    switch (signal_number) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "signal";
    }
}

/* Names the application before the process ends. The previous action (the
 * default, or a sanitizer's report) runs when the faulting instruction runs
 * again after this handler returns. */
static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    const ucontext_t *machine = context;
    uintptr_t pc = (uintptr_t)machine->uc_mcontext.gregs[REG_RIP];
    uintptr_t begin;
    uintptr_t end;
    bool in_image = roundie_elf_image_range(&begin, &end) && pc >= begin && pc < end;
    if (in_image || roundie_elf_on_executor()) {
        char report[FAULT_REPORT_BYTES];
        size_t used = append_text(report, 0, "E elf: application ");
        used = append_text(report, used, loaded_id[0] != '\0' ? loaded_id : "(unknown)");
        used = append_text(report, used, " faulted: ");
        used = append_text(report, used, signal_name(signal_number));
        if (in_image) {
            used = append_text(report, used, " at image+");
            used = append_number(report, used, pc - begin, 16);
        } else {
            used = append_text(report, used, " outside its image (in code it called, or a bad jump), pc ");
            used = append_number(report, used, pc, 16);
        }
        used = append_text(report, used, ", address ");
        used = append_number(report, used, (uintptr_t)info->si_addr, 16);
        used = append_text(report, used, "; the desktop ends. After a restart the launcher starts on the list.\n");
        ssize_t written = write(STDERR_FILENO, report, used);
        (void)written;
    }
    for (size_t i = 0; i < sizeof(fault_signals) / sizeof(fault_signals[0]); i++) {
        if (fault_signals[i] == signal_number) {
            sigaction(signal_number, &previous_actions[i], NULL);
        }
    }
    if (info->si_code <= 0) {
        raise(signal_number); /* sent, not caused by an instruction: no second fault */
    }
}

static void install_fault_handler(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(fault_signals) / sizeof(fault_signals[0]); i++) {
        if (sigaction(fault_signals[i], &action, &previous_actions[i]) != 0) {
            ESP_LOGW("elf", "fault reports unavailable for %s", signal_name(fault_signals[i]));
        }
    }
}

static void mark_loaded(const char *id)
{
    memcpy(loaded_id, id, strlen(id) + 1);
    FILE *marker = fopen(LOADED_MARKER, "w");
    if (marker == NULL || fputs(id, marker) == EOF || fclose(marker) != 0) {
        ESP_LOGW("elf", "%s: %s not written: %s", id, LOADED_MARKER, strerror(errno));
        return;
    }
    marked = true;
}

static void clear_loaded(void)
{
    if (marked && unlink(LOADED_MARKER) != 0 && errno != ENOENT) {
        ESP_LOGW("elf", "%s not removed: %s", LOADED_MARKER, strerror(errno));
    }
    marked = false;
}

void roundie_desktop_apps_enable(bool enabled)
{
    atomic_store(&native_apps, enabled);
    if (enabled) {
        ESP_LOGW("elf", "native applications enabled: trusted code in this process, validated but not isolated");
        install_fault_handler();
    }
}

bool roundie_desktop_apps_startup(void)
{
    char id[ROUNDIE_PACKAGE_ID] = {0};
    FILE *marker = fopen(LOADED_MARKER, "r");
    if (marker == NULL) {
        return false;
    }
    size_t length = fread(id, 1, sizeof(id) - 1, marker);
    fclose(marker);
    id[length] = '\0';
    /* main/main.c's message; the board then recovers through Safe Mode, the desktop
     * through this restart, and both start the next session on the list. */
    ESP_LOGW("elf", "Previous session failed with package %s loaded", id);
    marked = true;
    clear_loaded();
    return true;
}

void roundie_desktop_apps_forget_loaded(void)
{
    if (unlink(LOADED_MARKER) != 0 && errno != ENOENT) {
        ESP_LOGW("elf", "%s not removed: %s", LOADED_MARKER, strerror(errno));
    }
}

void roundie_desktop_apps_shutdown(void)
{
    clear_loaded(); /* a planned exit, like the board's planned restart */
}

bool roundie_desktop_apps_enabled(void)
{
    return atomic_load(&native_apps);
}

/* Flash write of the blob an unloaded instance wrote, as on the board. */
static void persist_storage(void)
{
    size_t length;
    if (storage_owner[0] == '\0' || roundie_elf_busy() || roundie_packages_busy() ||
        !roundie_elf_storage_take(storage_buffer, &length)) {
        return;
    }
    esp_err_t error = roundie_packages_data_write(storage_owner, storage_buffer, length);
    if (error != ESP_OK) {
        roundie_packages_status_t packages;
        roundie_packages_status(&packages);
        ESP_LOGW("app_storage", "%s: %zu bytes not saved: error %d (%s)", storage_owner, length,
                 (int)error, packages.error);
    }
    storage_owner[0] = '\0';
}

void roundie_desktop_apps_poll(int64_t now_us)
{
    roundie_elf_set_allowed(atomic_load(&native_apps) && !roundie_packages_busy());
    if (!roundie_elf_busy()) {
        clear_loaded();
        persist_storage();
    }
    int64_t since = roundie_elf_executor_busy_since();
    if (since != 0 && now_us - since >= STALL_REPORT_US && since != stall_reported) {
        stall_reported = since;
        roundie_elf_status_t status;
        roundie_elf_status(&status);
        ESP_LOGE("elf", "instance %" PRIu32 " has not returned for %" PRId64 " ms; native code cannot be "
                 "stopped safely, so the desktop keeps running without it and refuses new launches. "
                 "Restart the desktop to reclaim it.", status.instance, (now_us - since) / 1000);
    }
}

/* "storage-fault absent" (docs/virtual-hardware.md): as main/app_runtime.c
 * without a mounted storage partition, no package work is admitted. */
bool roundie_packages_allowed(void)
{
    return vhw_storage_fault() != VHW_STORAGE_ABSENT;
}

bool roundie_packages_runtime_busy(void)
{
    return roundie_elf_busy();
}

/* nftw passes no context: the walk of the calling thread. */
static _Thread_local uint64_t storage_used;

static int add_file_size(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)path;
    (void)walk;
    if (type == FTW_F) {
        storage_used += (uint64_t)info->st_size;
    }
    return 0;
}

/* The host's free space, or with "storage-limit" what is left of that limit
 * after the files under storage/. */
bool roundie_packages_capacity(size_t *free_bytes)
{
    vhw_state_t lab;
    vhw_get(&lab, roundie_time_us());
    if (lab.storage == VHW_STORAGE_ABSENT) {
        return false;
    }
    struct statvfs info;
    if (statvfs(".", &info) != 0) {
        return false;
    }
    uint64_t available = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
    if (lab.storage_limit_bytes != 0) {
        storage_used = 0;
        if (nftw("storage", add_file_size, 16, FTW_PHYS) != 0) {
            return false;
        }
        uint64_t left = lab.storage_limit_bytes > storage_used ? lab.storage_limit_bytes - storage_used : 0;
        if (left < available) {
            available = left;
        }
    }
    *free_bytes = (size_t)available;
    return true;
}

int64_t roundie_packages_clock(void)
{
    return roundie_time_us();
}

void *roundie_packages_allocate(size_t size)
{
    return malloc(size);
}

void roundie_packages_free(void *memory)
{
    free(memory);
}

int roundie_packages_execute(const char *id, uint8_t *bytes, size_t size, uint16_t abi)
{
    if (!atomic_load(&native_apps)) {
        ESP_LOGW("elf", "%s: native applications are disabled; start the desktop with --native-apps", id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    roundie_elf_set_allowed(!roundie_packages_busy());
    persist_storage(); /* the previous instance's blob, before reading this one */
    if (abi == 3) {
        size_t length = 0;
        esp_err_t read = roundie_packages_data_read(id, storage_buffer, sizeof(storage_buffer), &length);
        if (read != ESP_OK) {
            ESP_LOGW("app_storage", "%s: stored data unreadable (error %d); starting empty", id, (int)read);
        }
        if (!roundie_elf_storage_set(storage_buffer, length)) {
            return ESP_ERR_INVALID_STATE; /* an earlier blob is still unsaved */
        }
        memcpy(storage_owner, id, strlen(id) + 1);
    }
    mark_loaded(id);
    esp_err_t error = abi == 1 ? roundie_elf_submit(bytes, size) : roundie_elf_start(bytes, size);
    if (error != ESP_OK) {
        roundie_elf_storage_clear();
        clear_loaded();
    }
    return error;
}

/* Kept for the script's clock command: the source applications get. */
void roundie_desktop_set_app_clock_source(roundie_elf_clock_source_t source)
{
    atomic_store(&clock_source, source);
    roundie_elf_set_clock_source(source);
}

roundie_elf_clock_source_t roundie_desktop_app_clock_source(void)
{
    return atomic_load(&clock_source);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "UNKNOWN ERROR";
    }
}
