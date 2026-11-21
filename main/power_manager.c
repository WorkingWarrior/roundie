#include "power_manager.h"
#include "roundie_storage_mode.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_AUDIO
#include "roundie_audio.h"
#endif
#if CONFIG_ROUNDIE_POWER

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recovery_diagnostics.h"
#include "roundie_ota.h"
#include "roundie_power.h"
#include "power_shared.h"
#if CONFIG_ROUNDIE_APP_RUNTIME
#include "app_runtime.h"
#include "roundie_elf.h"
#include "roundie_launcher.h"
#endif
#if CONFIG_ROUNDIE_DEV_POWER
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "power";

/* System worker half of the power manager: admission, light sleep, restarts
 * and the RW1 development commands. The GUI half is power_gui.c. */

/* Touch INT must be high within a second after the GUI parked. */
#define WAKE_SOURCE_WAIT_US 1000000
/* Bound on unloading the home application before light sleep. */
#define HOME_PARK_WAIT_US 3000000

/* Worker only: first admission check of the pending sleep or restart request. */
static int64_t sleep_requested_us;
static int64_t command_requested_us;

static void report(const char *message)
{
    portENTER_CRITICAL(&roundie_power_shared.lock);
    snprintf(roundie_power_shared.message, sizeof(roundie_power_shared.message), "%s", message);
    roundie_power_shared.message_serial++;
    portEXIT_CRITICAL(&roundie_power_shared.lock);
}


static bool ota_idle(void)
{
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return ota.initialized && ota.running.state == ESP_OTA_IMG_VALID && ota.phase == ROUNDIE_OTA_IDLE;
}

static bool runtime_busy(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    return roundie_app_runtime_busy();
#else
    return false;
#endif
}

static bool audio_busy(void)
{
#if CONFIG_ROUNDIE_AUDIO
    return roundie_audio_busy();
#else
    return false;
#endif
}

static roundie_power_refusal_t sleep_refusal(roundie_board_t *board, bool check_wake_source)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    portENTER_CRITICAL(&roundie_power_shared.lock);
    uint32_t inhibited = roundie_power_inhibit_active(&roundie_power_shared.inhibitors);
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    roundie_power_conditions_t conditions = {
        .safe = boot.safe,
        .stable = boot.stage == ROUNDIE_BOOT_STABLE,
        .ota_idle = ota_idle(),
        .runtime_busy = runtime_busy() || roundie_storage_mode_active() || audio_busy(), /* USB would drop; audio would cut */
        .wake_source_active = check_wake_source && roundie_board_wake_source_active(board),
        .inhibited = (inhibited & ROUNDIE_POWER_INHIBIT_CPU_AWAKE) != 0U,
    };
    return roundie_power_sleep_refusal(&conditions);
}

static void restart(bool safe)
{
    if (runtime_busy()) {
        report("Zamknij aplikację");
        return;
    }
    if (roundie_recovery_active() || !ota_idle() || !roundie_recovery_restart_allowed()) {
        report("Zajęty: trwa aktualizacja");
        return;
    }
    if (safe) {
        /* Same persistent request as RS1 REQUEST on an idle runtime. */
        roundie_boot_enter_safe(ROUNDIE_SAFE_REQUEST);
        if (!roundie_boot_persistence_sync()) {
            report("Nie zapisano trybu awaryjnego");
            return;
        }
    }
    ESP_LOGI(TAG, "Power menu %s", safe ? "Safe Mode restart" : "restart");
    roundie_boot_planned_restart();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* Same planned restart as above, into USB storage mode: the one-shot request
 * is kept in RTC memory and read first thing after the restart. The caller has
 * already parked the home application. */
static const char *storage_refusal_text(roundie_storage_refusal_t refusal)
{
    switch (refusal) {
    case ROUNDIE_STORAGE_ALLOWED:
        return "";
    case ROUNDIE_STORAGE_REFUSED_ACTIVE:
        return "już włączony";
    case ROUNDIE_STORAGE_REFUSED_SAFE_MODE:
        return "tryb awaryjny";
    case ROUNDIE_STORAGE_REFUSED_NOT_STABLE:
        return "poczekaj chwilę";
    case ROUNDIE_STORAGE_REFUSED_OTA:
        return "trwa aktualizacja";
    case ROUNDIE_STORAGE_REFUSED_UNCONFIRMED:
        return "wersja niepotwierdzona";
    case ROUNDIE_STORAGE_REFUSED_APP:
        return "zamknij aplikację";
    }
    return "odmowa";
}

static void enter_storage_mode(void)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    const roundie_storage_conditions_t conditions = {
        .storage_mode = roundie_storage_mode_active(),
        .safe = boot.safe,
        .stable = boot.stage == ROUNDIE_BOOT_STABLE,
        .ota_idle = ota.initialized && ota.phase == ROUNDIE_OTA_IDLE,
        .image_valid = ota.initialized && ota.running.state == ESP_OTA_IMG_VALID,
        .app_busy = runtime_busy(),
    };
    roundie_storage_refusal_t refusal = roundie_storage_mode_refusal(&conditions);
    if (refusal != ROUNDIE_STORAGE_ALLOWED || !roundie_recovery_restart_allowed()) {
        char message[40];
        snprintf(message, sizeof(message), "Pamięć USB: %s",
                 refusal != ROUNDIE_STORAGE_ALLOWED ? storage_refusal_text(refusal) : "trwa aktualizacja");
        report(message);
        ESP_LOGW(TAG, "Storage mode refused: %s", message);
        return;
    }
    roundie_storage_mode_request(esp_app_get_description()->app_elf_sha256);
    ESP_LOGI(TAG, "Power menu: restart into USB storage mode");
    roundie_boot_planned_restart();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* Light sleep needs an idle runtime. The home application is unloaded for it
 * and launched again afterwards; any other application refuses sleep. */
static bool park_home(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    return roundie_launcher_park_home() == ESP_OK;
#else
    return false;
#endif
}

static void resume_home(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    roundie_launcher_resume_home();
#endif
}

static void finish_sleep(bool user_wake, roundie_power_refusal_t refusal, esp_err_t error)
{
    resume_home();
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_power_shared.phase = SLEEP_RESUMED;
    roundie_power_shared.user_wake = user_wake;
    roundie_power_shared.refusal = refusal;
    roundie_power_shared.sleep_error = error;
    portEXIT_CRITICAL(&roundie_power_shared.lock);
}

static void light_sleep(roundie_board_t *board, uint32_t limit_s)
{
    ESP_LOGI(TAG, "Entering light sleep; USB Serial/JTAG is unavailable until wake");
#if CONFIG_ROUNDIE_APP_RUNTIME
    /* Before the sleep: the executor rechecks its stack guard before any
     * native code runs after it (M9.1 fail closed, M8-B). */
    roundie_elf_guard_recheck();
#endif
    esp_err_t error = roundie_board_wake_arm(board);
    if (error == ESP_OK && limit_s > 0) {
        error = esp_sleep_enable_timer_wakeup((uint64_t)limit_s * 1000000U);
    }
    int64_t started_us = esp_timer_get_time();
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_pm_entered(&roundie_power_shared.pm, started_us);
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    if (error == ESP_OK) {
        error = esp_light_sleep_start();
    }
    int64_t slept_us = esp_timer_get_time() - started_us;
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (limit_s > 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    roundie_board_wake_disarm(board);
    /* Only a pure timer wake keeps the screen off; a failure shows it. */
    bool timer_only = causes == (1U << ESP_SLEEP_WAKEUP_TIMER);
    bool user_wake = error != ESP_OK || !timer_only;
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_power_shared.wake_causes = causes;
    roundie_power_shared.last_sleep_us = slept_us;
    if (error == ESP_OK) {
        roundie_power_shared.sleeps++;
    }
    roundie_pm_woke(&roundie_power_shared.pm, causes, user_wake, error != ESP_OK, slept_us, esp_timer_get_time());
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    finish_sleep(user_wake, ROUNDIE_POWER_ALLOWED, error);
    ESP_LOGI(TAG, "Light sleep %" PRId64 " ms, wake causes 0x%08" PRIx32 ", result %s",
             slept_us / 1000, causes, esp_err_to_name(error));
}

void roundie_power_worker_poll(int64_t now_us)
{
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_board_t *board = roundie_power_shared.board;
    sleep_phase_t phase = roundie_power_shared.phase;
    uint32_t limit_s = roundie_power_shared.sleep_limit_s;
    int64_t parked_at_us = roundie_power_shared.parked_at_us;
    command_t command = roundie_power_shared.command;
    roundie_power_shared.command = COMMAND_NONE;
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    if (board == NULL) {
        return;
    }
    if (command != COMMAND_NONE) {
        if (command_requested_us == 0) {
            command_requested_us = now_us;
        }
        if (runtime_busy() && now_us - command_requested_us < HOME_PARK_WAIT_US && park_home()) {
            portENTER_CRITICAL(&roundie_power_shared.lock);
            roundie_power_shared.command = command; /* again once the home application has unloaded */
            portEXIT_CRITICAL(&roundie_power_shared.lock);
            return;
        }
        command_requested_us = 0;
        if (command == COMMAND_STORAGE) {
            enter_storage_mode();
        } else {
            restart(command == COMMAND_SAFE_MODE);
        }
        resume_home(); /* reached only when the restart was refused */
    }
    if (phase == SLEEP_REQUESTED) {
        /* The finger that chose Sleep may still be on the panel. */
        roundie_power_refusal_t refusal = sleep_refusal(board, false);
        if (sleep_requested_us == 0) {
            sleep_requested_us = now_us;
        }
        if (refusal == ROUNDIE_POWER_REFUSED_APP && now_us - sleep_requested_us < HOME_PARK_WAIT_US &&
            park_home()) {
            return; /* checked again once the home application has unloaded */
        }
        sleep_requested_us = 0;
        if (refusal != ROUNDIE_POWER_ALLOWED) {
            resume_home();
        }
        portENTER_CRITICAL(&roundie_power_shared.lock);
        roundie_power_shared.refusal = refusal;
        roundie_power_shared.phase = refusal == ROUNDIE_POWER_ALLOWED ? SLEEP_PARK : SLEEP_IDLE;
        if (refusal != ROUNDIE_POWER_ALLOWED) {
            roundie_pm_cancel(&roundie_power_shared.pm, refusal, now_us);
        }
        portEXIT_CRITICAL(&roundie_power_shared.lock);
        if (refusal != ROUNDIE_POWER_ALLOWED) {
            char message[40];
            snprintf(message, sizeof(message), "Uśpienie odrzucone: %s", roundie_power_refusal_name(refusal));
            report(message);
            ESP_LOGW(TAG, "%s", message);
        }
        return;
    }
    if (phase != SLEEP_PARKED) {
        return;
    }
    roundie_power_refusal_t refusal = sleep_refusal(board, true);
    if (refusal == ROUNDIE_POWER_REFUSED_WAKE_SOURCE && now_us - parked_at_us < WAKE_SOURCE_WAIT_US) {
        return;
    }
    if (refusal != ROUNDIE_POWER_ALLOWED) {
        ESP_LOGW(TAG, "Sleep aborted: %s", roundie_power_refusal_name(refusal));
        portENTER_CRITICAL(&roundie_power_shared.lock);
        roundie_pm_cancel(&roundie_power_shared.pm, refusal, now_us);
        portEXIT_CRITICAL(&roundie_power_shared.lock);
        finish_sleep(true, refusal, ESP_ERR_INVALID_STATE);
        return;
    }
    light_sleep(board, limit_s);
}

#if CONFIG_ROUNDIE_DEV_POWER
static const char *phase_name(sleep_phase_t phase)
{
    switch (phase) {
    case SLEEP_IDLE: return "idle";
    case SLEEP_REQUESTED: return "requested";
    case SLEEP_PARK: return "park";
    case SLEEP_PARKED: return "parked";
    case SLEEP_RESUMED: return "resumed";
    }
    return "unknown";
}

#endif

bool roundie_power_diagnostics_hold(uint32_t *handle, uint32_t kinds, uint32_t seconds)
{
    const int64_t now_us = esp_timer_get_time();
    bool held = false;
    portENTER_CRITICAL(&roundie_power_shared.lock);
    if (*handle != 0) {
        /* An expired handle is already released: STALE is expected then. */
        (void)roundie_power_inhibit_release(&roundie_power_shared.inhibitors, *handle, now_us);
        *handle = 0;
    }
    if (seconds > 0) {
        roundie_power_inhibit_handle_t acquired = 0;
        held = roundie_power_inhibit_acquire(&roundie_power_shared.inhibitors, ROUNDIE_POWER_OWNER_DIAGNOSTICS,
                                             kinds, now_us, (int64_t)seconds * 1000000, &acquired) ==
               ROUNDIE_POWER_INHIBIT_OK;
        if (held) {
            *handle = acquired;
        }
    }
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    return held;
}

bool roundie_power_request_timed_sleep(uint32_t limit_s)
{
    if (limit_s < 1 || limit_s > 3600) {
        return false;
    }
    bool accepted = false;
    portENTER_CRITICAL(&roundie_power_shared.lock);
    if (roundie_power_shared.board != NULL && roundie_power_shared.phase == SLEEP_IDLE &&
        roundie_power_shared.command == COMMAND_NONE && roundie_power_shared.state == ROUNDIE_POWER_SCREEN_OFF &&
        roundie_power_shared.screen_request == SCREEN_REQUEST_NONE &&
        roundie_pm_request_light_sleep(&roundie_power_shared.pm, esp_timer_get_time()) == ROUNDIE_POWER_ALLOWED) {
        roundie_power_shared.phase = SLEEP_REQUESTED;
        roundie_power_shared.sleep_limit_s = limit_s;
        accepted = true;
    }
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    return accepted;
}

void roundie_power_battery_alert(uint8_t alert)
{
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_power_shared.battery_alert = alert;
    roundie_power_shared.battery_alert_serial++;
    if (alert != 0 && roundie_power_shared.board != NULL) {
        roundie_power_shared.screen_request = SCREEN_REQUEST_ON;
    }
    portEXIT_CRITICAL(&roundie_power_shared.lock);
}

#if CONFIG_ROUNDIE_DEV_POWER
void roundie_power_command(char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    roundie_power_inhibit_handle_t handle = 0;
    size_t length = strlen(line);
    if (length >= 14 && memcmp(line, "!RW1 ", 5) == 0 && line[13] == ' ') {
        bool valid_id = true;
        for (size_t i = 5; i < 13; i++) {
            valid_id = valid_id && ((line[i] >= '0' && line[i] <= '9') ||
                                    (line[i] >= 'a' && line[i] <= 'f'));
        }
        if (valid_id) {
            memcpy(id, line + 5, 8);
        }
        const char *operation = line + 14;
        portENTER_CRITICAL(&roundie_power_shared.lock);
        bool bound = roundie_power_shared.board != NULL;
        bool idle = roundie_power_shared.phase == SLEEP_IDLE;
        portEXIT_CRITICAL(&roundie_power_shared.lock);
        if (!valid_id || discard || length >= 64) {
            error = ESP_ERR_INVALID_ARG;
        } else if (strcmp(operation, "STATUS") == 0) {
            error = ESP_OK;
        } else if (!bound) {
            error = ESP_ERR_INVALID_STATE;
        } else if (strcmp(operation, "SCREEN OFF") == 0 || strcmp(operation, "SCREEN ON") == 0) {
            portENTER_CRITICAL(&roundie_power_shared.lock);
            roundie_power_shared.screen_request = operation[8] == 'F' ? SCREEN_REQUEST_OFF : SCREEN_REQUEST_ON;
            portEXIT_CRITICAL(&roundie_power_shared.lock);
            error = ESP_OK;
        } else if (strcmp(operation, "STORAGE") == 0) {
            /* The same request as the power menu item, so the parking path can be tested. */
            portENTER_CRITICAL(&roundie_power_shared.lock);
            bool accepted = roundie_power_shared.command == COMMAND_NONE && roundie_power_shared.phase == SLEEP_IDLE;
            if (accepted) {
                roundie_power_shared.command = COMMAND_STORAGE;
            }
            portEXIT_CRITICAL(&roundie_power_shared.lock);
            error = accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
        } else if (strncmp(operation, "SLEEP ", 6) == 0) {
            /* Diagnostics always carry a timer so the host gets USB back. */
            char *end = NULL;
            long seconds = strtol(operation + 6, &end, 10);
            if (end == NULL || *end != '\0' || seconds < 1 || seconds > 3600) {
                error = ESP_ERR_INVALID_ARG;
            } else if (!idle) {
                error = ESP_ERR_INVALID_STATE;
            } else {
                portENTER_CRITICAL(&roundie_power_shared.lock);
                bool accepted = roundie_power_shared.phase == SLEEP_IDLE &&
                    roundie_pm_request_light_sleep(&roundie_power_shared.pm, esp_timer_get_time()) ==
                        ROUNDIE_POWER_ALLOWED;
                if (accepted) {
                    roundie_power_shared.phase = SLEEP_REQUESTED;
                    roundie_power_shared.sleep_limit_s = (uint32_t)seconds;
                }
                portEXIT_CRITICAL(&roundie_power_shared.lock);
                error = accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
            }
        } else if (strcmp(operation, "DEEPSLEEP") == 0) {
            /* M8-B: deep sleep is modelled but not validated on the 1.85B
             * (no confirmed autonomous wake and USB recovery): the request
             * is recorded and refused, never executed. */
            portENTER_CRITICAL(&roundie_power_shared.lock);
            int64_t now_us = esp_timer_get_time();
            bool modelled = roundie_power_shared.phase == SLEEP_IDLE &&
                roundie_pm_request_deep_sleep(&roundie_power_shared.pm, now_us) == ROUNDIE_POWER_ALLOWED;
            if (modelled) {
                roundie_pm_cancel(&roundie_power_shared.pm, ROUNDIE_POWER_REFUSED_UNSUPPORTED, now_us);
            }
            portEXIT_CRITICAL(&roundie_power_shared.lock);
            error = modelled ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
        } else if (strncmp(operation, "INHIBIT ", 8) == 0) {
            /* INHIBIT <kinds 1..7> <seconds 1..600>: a diagnostics-owned
             * inhibitor, for tests of admission, release and expiry. */
            char *end = NULL;
            long kinds = strtol(operation + 8, &end, 10);
            long seconds = 0;
            if (end != NULL && *end == ' ') {
                seconds = strtol(end + 1, &end, 10);
            }
            if (end == NULL || *end != '\0' || kinds < 1 || kinds > (long)ROUNDIE_POWER_INHIBIT_KINDS ||
                seconds < 1 || seconds > 600) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                portENTER_CRITICAL(&roundie_power_shared.lock);
                roundie_power_inhibit_result_t result = roundie_power_inhibit_acquire(
                    &roundie_power_shared.inhibitors, ROUNDIE_POWER_OWNER_DIAGNOSTICS, (uint32_t)kinds,
                    esp_timer_get_time(), (int64_t)seconds * 1000000, &handle);
                portEXIT_CRITICAL(&roundie_power_shared.lock);
                error = result == ROUNDIE_POWER_INHIBIT_OK ? ESP_OK : ESP_ERR_NO_MEM;
            }
        } else if (strncmp(operation, "RELEASE ", 8) == 0) {
            char *end = NULL;
            unsigned long value = strtoul(operation + 8, &end, 16);
            if (end == NULL || *end != '\0' || value == 0UL || value > UINT32_MAX) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                portENTER_CRITICAL(&roundie_power_shared.lock);
                roundie_power_inhibit_result_t result = roundie_power_inhibit_release(
                    &roundie_power_shared.inhibitors, (roundie_power_inhibit_handle_t)value, esp_timer_get_time());
                portEXIT_CRITICAL(&roundie_power_shared.lock);
                handle = (roundie_power_inhibit_handle_t)value;
                error = result == ROUNDIE_POWER_INHIBIT_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
            }
        } else {
            error = ESP_ERR_INVALID_ARG;
        }
    }
    /* Admission as a sleep request would see it now (wake source excluded). */
    roundie_power_refusal_t admission = ROUNDIE_POWER_REFUSED_NOT_STABLE;
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_board_t *board = roundie_power_shared.board;
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    if (board != NULL) {
        admission = sleep_refusal(board, false);
    }
#if CONFIG_ROUNDIE_APP_RUNTIME
    roundie_elf_guard_status_t guard;
    roundie_elf_guard_status(&guard);
#else
    roundie_elf_guard_status_t guard = {0};
#endif
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&roundie_power_shared.lock);
    roundie_power_state_t state = roundie_power_shared.state;
    sleep_phase_t phase = roundie_power_shared.phase;
    roundie_power_refusal_t refusal = roundie_power_shared.refusal;
    esp_err_t sleep_error = roundie_power_shared.sleep_error;
    uint32_t sleeps = roundie_power_shared.sleeps;
    uint32_t screen_offs = roundie_power_shared.screen_offs;
    uint32_t wake_causes = roundie_power_shared.wake_causes;
    int64_t last_sleep_us = roundie_power_shared.last_sleep_us;
    bool bound = roundie_power_shared.board != NULL;
    roundie_pm_t pm = roundie_power_shared.pm;
    uint32_t owners = 0;
    uint32_t held = roundie_power_inhibit_held(&roundie_power_shared.inhibitors, &owners);
    uint32_t kinds = roundie_power_inhibit_active(&roundie_power_shared.inhibitors);
    uint32_t stale = roundie_power_shared.inhibitors.stale_releases;
    uint32_t expired = roundie_power_shared.inhibitors.expired;
    uint32_t full = roundie_power_shared.inhibitors.full;
    portEXIT_CRITICAL(&roundie_power_shared.lock);
    static const char *const guard_names[] = {"pending", "armed", "failed"};
    char response[1024];
    int length_out = snprintf(response, sizeof(response),
        "\n!RW1 %s %d {\"bound\":%u,\"state\":\"%s\",\"phase\":\"%s\",\"refusal\":\"%s\","
        "\"sleep_error\":%d,\"sleeps\":%" PRIu32 ",\"last_sleep_ms\":%" PRId64 ",\"wake_causes\":%" PRIu32 ","
        "\"screen_offs\":%" PRIu32 ",\"timeout_s\":%d,\"safety_s\":%d,"
        "\"pm\":\"%s\",\"pm_from\":\"%s\",\"pm_age_ms\":%" PRId64 ",\"transitions\":%" PRIu32 ","
        "\"rejected\":%" PRIu32 ",\"attempts\":%" PRIu32 ",\"refused\":%" PRIu32 ",\"last_refusal\":\"%s\","
        "\"admission\":\"%s\",\"light_sleeps\":%" PRIu32 ",\"user_wakes\":%" PRIu32 ",\"timer_wakes\":%" PRIu32 ","
        "\"sleep_errors\":%" PRIu32 ",\"t_active_ms\":%" PRId64 ",\"t_screen_off_ms\":%" PRId64 ","
        "\"t_light_sleep_ms\":%" PRId64 ",\"t_pending_ms\":%" PRId64 ",\"t_resuming_ms\":%" PRId64 ","
        "\"suspend_ms\":%" PRId64 ",\"suspend_max_ms\":%" PRId64 ",\"resume_ms\":%" PRId64 ",\"resume_max_ms\":%" PRId64 ","
        "\"inhibitors\":%" PRIu32 ",\"inhibit_kinds\":%" PRIu32 ",\"inhibit_owners\":%" PRIu32 ","
        "\"inhibit_stale\":%" PRIu32 ",\"inhibit_expired\":%" PRIu32 ",\"inhibit_full\":%" PRIu32 ","
        "\"handle\":\"%08" PRIx32 "\",\"guard\":\"%s\",\"guard_pending\":%u,\"guard_checks\":%" PRIu32 ","
        "\"guard_failures\":%" PRIu32 ",\"guard_lost\":%" PRIu32 "}\n",
        id, (int)error, bound, roundie_power_state_name(state), phase_name(phase),
        roundie_power_refusal_name(refusal), (int)sleep_error, sleeps, last_sleep_us / 1000,
        wake_causes, screen_offs, CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS,
        CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS,
        roundie_pm_state_name(pm.state), roundie_pm_state_name(pm.last_from),
        (now_us - pm.last_transition_us) / 1000, pm.transitions, pm.rejected, pm.attempts, pm.refused,
        roundie_power_refusal_name(pm.last_refusal), roundie_power_refusal_name(admission), pm.light_sleeps,
        pm.user_wakes, pm.timer_wakes, pm.sleep_errors,
        roundie_pm_time_in(&pm, ROUNDIE_PM_ACTIVE, now_us) / 1000,
        roundie_pm_time_in(&pm, ROUNDIE_PM_SCREEN_OFF, now_us) / 1000,
        roundie_pm_time_in(&pm, ROUNDIE_PM_LIGHT_SLEEP, now_us) / 1000,
        roundie_pm_time_in(&pm, ROUNDIE_PM_LIGHT_SLEEP_PENDING, now_us) / 1000,
        roundie_pm_time_in(&pm, ROUNDIE_PM_RESUMING, now_us) / 1000,
        pm.last_suspend_us / 1000, pm.max_suspend_us / 1000, pm.last_resume_us / 1000, pm.max_resume_us / 1000,
        held, kinds, owners, stale, expired, full, handle,
        guard_names[guard.state <= ROUNDIE_ELF_GUARD_FAILED ? guard.state : 0], guard.recheck_pending,
        guard.checks, guard.failures, guard.lost);
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
}
#endif
#endif
