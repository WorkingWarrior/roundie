/* Execute the actual boot orchestration in normal and armed compilations.
 * IDF/manager calls are spies; manager eligibility and hold use separate tests.
 * A fresh process per case resets firmware-static state without a test reset API.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_diagnostics.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"

static roundie_ota_status_t current;
static unsigned boot_results;
static unsigned holds;
static unsigned restarts;
static unsigned drains;
static bool reported_pass;
static char logs[8192];
static size_t log_used;

void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    int length = vsnprintf(logs + log_used, sizeof(logs) - log_used, format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(logs) - log_used);
    log_used += (size_t)length;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "OK" : "error";
}

void esp_restart(void)
{
    restarts++;
    /* Deliberately return: even this unexpected path must not fall through
     * to normal confirmation or explicit INVALID/reboot policy. */
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 100);
}

esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks)
{
    assert(ticks == 100);
    drains++;
    return ESP_OK;
}

esp_err_t roundie_ota_init(void)
{
    return ESP_OK;
}

void roundie_ota_status(roundie_ota_status_t *out)
{
    *out = current;
}

void roundie_ota_tick(int64_t now_us)
{
    (void)now_us;
}

bool roundie_ota_test_reset_allowed(void)
{
    return current.initialized && current.running.state == ESP_OTA_IMG_PENDING_VERIFY &&
           current.rollback_possible;
}

esp_err_t roundie_ota_test_hold_unconfirmed(esp_err_t reason)
{
    assert(reason != ESP_OK && !current.boot_checked);
    holds++;
    current.boot_checked = true;
    current.boot_passed = false;
    current.last_error = reason;
    return ESP_OK;
}

esp_err_t roundie_ota_boot_result(bool passed)
{
    assert(!current.boot_checked);
    boot_results++;
    reported_pass = passed;
    current.boot_checked = true;
    current.boot_passed = passed;
    return ESP_OK;
}

const char *roundie_ota_image_state_name(int state)
{
    return state == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY" : "other";
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    int state = atoi(argv[1]);
    bool fallback = atoi(argv[2]) != 0;
    const char *health = argv[3];
    current = (roundie_ota_status_t){.initialized = true, .rollback_possible = fallback};
    current.running.state = state;
    roundie_ota_diagnostics_init();
    if (strcmp(health, "startup") == 0) {
        roundie_ota_startup_failed();
    } else {
        if (strcmp(health, "timeout") != 0) {
            roundie_ota_gui_health(strcmp(health, "pass") == 0);
        }
        roundie_ota_poll(1000000);
        roundie_ota_poll(6000000);
        roundie_ota_poll(7000000);
    }
#if CONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET
    bool should_reset = state == ESP_OTA_IMG_PENDING_VERIFY && fallback && strcmp(health, "pass") == 0;
    assert(restarts == (unsigned)should_reset);
    assert(drains == (unsigned)should_reset);
    assert(holds == 1 && boot_results == 0 && !current.boot_passed);
    assert(current.running.state == state); /* No metadata confirmation/rejection. */
    assert(!roundie_ota_restart_allowed());
    assert((strstr(logs, "ARMED core self-test PASS") != NULL) == should_reset);
#else
    assert(restarts == 0 && drains == 0 && holds == 0 && boot_results == 1);
    assert(reported_pass == (strcmp(health, "pass") == 0));
#endif
    puts("boot gate scenario PASS (actual orchestration, mocked system calls)");
    return 0;
}
