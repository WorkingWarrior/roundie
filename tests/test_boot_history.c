/* Diagnostic boot history ring (components/roundie_boot_guard/history.c)
 * against an in-memory NVS mock. */
#include <assert.h>
#include <string.h>
#include "nvs.h"
#include "esp_system.h"
#include "roundie_boot_guard.h"

static unsigned char stored[256];
static size_t stored_size;
static bool present;
static bool open_ok = true;
static bool write_ok = true;
static esp_err_t read_error = ESP_OK;
static unsigned writes;
static esp_reset_reason_t raw_reset;

esp_err_t nvs_open(const char *name, unsigned mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "boot_history") == 0 && mode == NVS_READWRITE);
    *handle = 7;
    return open_ok ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    assert(handle == 7 && strcmp(key, "history") == 0);
    if (read_error != ESP_OK) {
        return read_error;
    }
    if (!present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*size < stored_size) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(data, stored, stored_size);
    *size = stored_size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 7 && strcmp(key, "history") == 0 && size <= sizeof(stored));
    writes++;
    if (!write_ok) {
        return ESP_FAIL;
    }
    memcpy(stored, data, size);
    stored_size = size;
    present = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 7);
    return write_ok ? ESP_OK : ESP_FAIL;
}
esp_reset_reason_t esp_reset_reason(void) { return raw_reset; }
void esp_restart(void) {}

static roundie_boot_history_t boot(esp_reset_reason_t reason, uint16_t vbat)
{
    raw_reset = reason;
    roundie_boot_history_record(true);
    roundie_boot_history_battery(vbat);
    roundie_boot_history_t history;
    roundie_boot_history_get(&history);
    return history;
}

int main(void)
{
    /* First boot creates the record: one write for the reason, one for the voltage. */
    roundie_boot_history_t h = boot(ESP_RST_POWERON, 3900);
    assert(h.state == ROUNDIE_HISTORY_OK && h.boots == 1 && h.brownouts == 0 && h.count == 1);
    assert(h.entries[0].boot == 1 && h.entries[0].raw_reset == ESP_RST_POWERON && h.entries[0].vbat_mv == 3900);
    assert(writes == 2);
    /* Only the first voltage of a boot is kept. */
    roundie_boot_history_battery(3000);
    roundie_boot_history_get(&h);
    assert(h.entries[0].vbat_mv == 3900 && writes == 2);

    /* Brownouts count; a boot without a gauge reading keeps 0 mV and writes once. */
    writes = 0;
    h = boot(ESP_RST_BROWNOUT, 0);
    assert(h.boots == 2 && h.brownouts == 1 && h.count == 2 && writes == 1);
    assert(h.entries[0].raw_reset == ESP_RST_BROWNOUT && h.entries[0].vbat_mv == 0);
    assert(h.entries[1].raw_reset == ESP_RST_POWERON);
    h = boot(ESP_RST_PWR_GLITCH, 3500);
    assert(h.brownouts == 1 && h.entries[0].raw_reset == ESP_RST_PWR_GLITCH);

    /* The ring keeps the newest eight, newest first. */
    for (unsigned i = 0; i < 10; i++) {
        h = boot(ESP_RST_PANIC, (uint16_t)(3600 + i));
    }
    assert(h.boots == 13 && h.count == ROUNDIE_BOOT_HISTORY_ENTRIES);
    for (unsigned i = 0; i < ROUNDIE_BOOT_HISTORY_ENTRIES; i++) {
        assert(h.entries[i].boot == 13 - i && h.entries[i].vbat_mv == 3609 - i);
    }

    /* A malformed record is kept as found and nothing is recorded. */
    unsigned char before[sizeof(stored)];
    stored[20] ^= 0x55U;
    memcpy(before, stored, sizeof(stored));
    writes = 0;
    h = boot(ESP_RST_SW, 3700);
    assert(h.state == ROUNDIE_HISTORY_MALFORMED && h.count == 0 && writes == 0);
    assert(memcmp(before, stored, sizeof(stored)) == 0);
    stored[20] ^= 0x55U;
    stored_size -= 4;
    h = boot(ESP_RST_SW, 3700);
    assert(h.state == ROUNDIE_HISTORY_MALFORMED && writes == 0);
    stored_size += 4;

    /* No NVS, an open error or a read error: unavailable, no write. */
    roundie_boot_history_record(false);
    roundie_boot_history_get(&h);
    assert(h.state == ROUNDIE_HISTORY_UNAVAILABLE && writes == 0);
    open_ok = false;
    h = boot(ESP_RST_SW, 3700);
    assert(h.state == ROUNDIE_HISTORY_UNAVAILABLE && writes == 0);
    open_ok = true;
    read_error = ESP_FAIL;
    h = boot(ESP_RST_SW, 3700);
    assert(h.state == ROUNDIE_HISTORY_UNAVAILABLE && writes == 0);
    read_error = ESP_OK;

    /* A failed write reports unavailable; the valid record resumes next boot. */
    write_ok = false;
    h = boot(ESP_RST_SW, 3700);
    assert(h.state == ROUNDIE_HISTORY_UNAVAILABLE);
    write_ok = true;
    h = boot(ESP_RST_SW, 3800);
    assert(h.state == ROUNDIE_HISTORY_OK && h.boots == 14 && h.entries[0].vbat_mv == 3800);
    return 0;
}
