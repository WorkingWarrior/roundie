/* Actual RO1 controller, mocked manager/USB. Assert mutations never reach the
 * OTA manager while native ownership remains, including loaded idle states. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/ota_diagnostics.c"

static bool app_busy;
static bool safe_latched;
static roundie_ota_operation_t next_operation = ROUNDIE_OTA_BEGIN;
static unsigned mutations;
static roundie_ota_status_t backend;
static char reply[1024];
void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
static const char *busy_reason = "app";
bool roundie_app_runtime_busy(void) { return app_busy; }
const char *roundie_app_runtime_busy_reason(void) { return app_busy ? busy_reason : NULL; }
void roundie_ota_status(roundie_ota_status_t *out) { *out = backend; }
bool roundie_recovery_active(void) { return false; }
bool roundie_recovery_restart_allowed(void) { return false; }
bool roundie_recovery_confirmation_allowed(int ota_state)
{
    assert(ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    return !safe_latched;
}
void roundie_boot_planned_restart(void) { assert(!app_busy); }
void esp_restart(void) { assert(!app_busy); }
void vTaskDelay(unsigned ticks) { (void)ticks; }
unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 5000; }
esp_err_t usb_serial_jtag_wait_tx_done(unsigned ticks) { (void)ticks; return ESP_OK; }
int usb_serial_jtag_write_bytes(const void *data, size_t length, unsigned ticks)
{
    (void)ticks;
    assert(length < sizeof(reply));
    memcpy(reply, data, length);
    reply[length] = 0;
    return (int)length;
}
const char *roundie_ota_phase_name(roundie_ota_phase_t phase) { (void)phase; return "IDLE"; }
const char *roundie_ota_image_state_name(int state) { (void)state; return "VALID"; }
esp_err_t roundie_ota_parse(char *line, bool discard, roundie_ota_command_t *out)
{
    (void)line;
    (void)discard;
    *out = (roundie_ota_command_t){.id = "12345678", .operation = next_operation};
    return ESP_OK;
}
esp_err_t roundie_ota_execute(const roundie_ota_command_t *command)
{
    assert(command->operation == ROUNDIE_OTA_BEGIN || command->operation == ROUNDIE_OTA_ACTIVATE);
    mutations++;
    return ESP_OK;
}

int main(void)
{
    backend.initialized = backend.boot_checked = backend.boot_passed = true;
    backend.running.state = ESP_OTA_IMG_VALID;
    app_busy = true;
    char line[] = "controller parser is tested separately";
    roundie_ota_command(line, false);
    int code;
    assert(sscanf(reply, "\n!RO1 12345678 %d", &code) == 1);
    assert(mutations == 0 && code == ESP_ERR_INVALID_STATE);
    assert(strstr(reply, "\"refusal\":\"app\"}") != NULL);
    assert(!roundie_ota_restart_allowed());
    /* P1 fix 3: each precondition is named in the reply and keeps 259. */
    const char *reasons[] = {"transfer", "package"};
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        busy_reason = reasons[i];
        roundie_ota_command(line, false);
        char expected[48];
        snprintf(expected, sizeof(expected), "\"refusal\":\"%s\"}", reasons[i]);
        assert(sscanf(reply, "\n!RO1 12345678 %d", &code) == 1);
        assert(mutations == 0 && code == ESP_ERR_INVALID_STATE && strstr(reply, expected) != NULL);
    }
    app_busy = false;
    /* An io_worker step in progress is transient: BUSY (503), not executed. */
    roundie_storage_claim_init();
    roundie_storage_claim_io_enter();
    roundie_ota_command(line, false);
    assert(sscanf(reply, "\n!RO1 12345678 %d", &code) == 1);
    assert(mutations == 0 && code == ROUNDIE_IO_BUSY_CODE && strstr(reply, "\"refusal\":\"io\"}") != NULL);
    roundie_storage_claim_io_leave();
    roundie_ota_command(line, false);
    assert(mutations == 1 && strstr(reply, "12345678 0 ") != NULL);
    assert(strstr(reply, "\"refusal\":\"none\"}") != NULL);
    roundie_storage_claim_ota_release();
    assert(roundie_ota_restart_allowed());
    /* M9.1: an image started from a latched Safe Mode can never confirm, so
     * BEGIN and ACTIVATE are refused before they reach the manager. */
    safe_latched = true;
    roundie_ota_operation_t refused[] = {ROUNDIE_OTA_BEGIN, ROUNDIE_OTA_ACTIVATE};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        next_operation = refused[i];
        roundie_ota_command(line, false);
        assert(sscanf(reply, "\n!RO1 12345678 %d", &code) == 1);
        assert(mutations == 1 && code == ESP_ERR_INVALID_STATE);
        assert(strstr(reply, "\"refusal\":\"safe-mode\"}") != NULL);
    }
    safe_latched = false;
    roundie_ota_command(line, false);
    assert(mutations == 2 && strstr(reply, "12345678 0 ") != NULL);
    puts("RO1 controller exclusion PASS: manager mutation and restart denied while native owner exists");
}
