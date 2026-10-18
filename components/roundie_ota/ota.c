#include "roundie_ota.h"

#include <string.h>
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"

#define CHUNK_MAX 512
#define PREFIX_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define SESSION_TIMEOUT_US INT64_C(15000000)
#define READY_TIMEOUT_US INT64_C(300000000)

static struct {
    roundie_ota_status_t status;
    const esp_partition_t *running;
    const esp_partition_t *target;
    esp_ota_handle_t handle;
    bool handle_open;
    uint8_t expected[32];
    psa_hash_operation_t hash;
    int64_t last_activity_us;
    int64_t now_us;
} manager;

static bool descriptor_valid(const esp_app_desc_t *desc);

static void hex_digest(const uint8_t *data, char *text)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        text[i * 2] = digits[data[i] >> 4];
        text[i * 2 + 1] = digits[data[i] & 15];
    }
    text[64] = '\0';
}

static bool image_valid(const esp_partition_t *partition, uint32_t *size)
{
    if (partition == NULL) {
        return false;
    }
    esp_partition_pos_t position = {.offset = partition->address, .size = partition->size};
    esp_image_metadata_t metadata = {0};
    bool valid = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK;
    *size = valid ? metadata.image_len : 0;
    return valid;
}

static void slot_info(const esp_partition_t *partition, roundie_ota_slot_t *out)
{
    *out = (roundie_ota_slot_t){.state = -2};
    if (partition == NULL) {
        return;
    }
    memcpy(out->label, partition->label, sizeof(partition->label));
    out->address = partition->address;
    out->size = partition->size;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) == ESP_OK) {
        out->state = (int)state;
    }
    out->image_valid = image_valid(partition, &out->image_size);
    esp_app_desc_t desc;
    if (out->image_valid && esp_ota_get_partition_description(partition, &desc) == ESP_OK) {
        out->compatible = descriptor_valid(&desc);
        memcpy(out->version, desc.version, sizeof(desc.version));
        out->secure_version = desc.secure_version;
        hex_digest(desc.app_elf_sha256, out->elf);
    }
}

static void refresh(void)
{
    slot_info(manager.running, &manager.status.running);
    slot_info(manager.target, &manager.status.other);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    memset(manager.status.boot, 0, sizeof(manager.status.boot));
    if (boot != NULL) {
        memcpy(manager.status.boot, boot->label, sizeof(boot->label));
    }
    manager.status.rollback_possible = manager.status.other.image_valid &&
        manager.status.other.compatible &&
        manager.status.other.state == ESP_OTA_IMG_VALID &&
        esp_ota_check_rollback_is_possible();
}

static esp_err_t fail(esp_err_t error)
{
    if (manager.handle_open) {
        esp_err_t cleanup = esp_ota_abort(manager.handle);
        manager.handle_open = false;
        if (cleanup != ESP_OK) {
            error = cleanup;
        }
    }
    psa_hash_abort(&manager.hash);
    manager.status.session = 0;
    manager.status.phase = ROUNDIE_OTA_IDLE;
    manager.status.last_error = error;
    manager.status.digest[0] = '\0';
    return error;
}

static bool is_slot(const esp_partition_t *p, unsigned subtype, uint32_t address)
{
    return p != NULL && p->type == ESP_PARTITION_TYPE_APP &&
           p->subtype == subtype && p->address == address && p->size == 0x380000;
}

esp_err_t roundie_ota_init(void)
{
    if (manager.handle_open) {
        return ESP_ERR_INVALID_STATE;
    }
    manager = (typeof(manager)){0};
    manager.status.running.state = -2;
    manager.status.other.state = -2;
    manager.running = esp_ota_get_running_partition();
    const esp_partition_t *a = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const esp_partition_t *b = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    const esp_partition_t *data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    if (!is_slot(a, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000) ||
        !is_slot(b, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x390000) ||
        data == NULL || data->address != 0xd000 || data->size != 0x2000 ||
        (manager.running != a && manager.running != b)) {
        return fail(ESP_ERR_NOT_SUPPORTED);
    }
    manager.target = manager.running == a ? b : a;
    manager.status.initialized = true;
    refresh();
    return ESP_OK;
}

void roundie_ota_status(roundie_ota_status_t *out)
{
    *out = manager.status;
}

bool roundie_ota_test_reset_allowed(void)
{
    return manager.status.initialized &&
           manager.status.running.state == ESP_OTA_IMG_PENDING_VERIFY &&
           manager.status.rollback_possible;
}

esp_err_t roundie_ota_test_hold_unconfirmed(esp_err_t reason)
{
    if (!manager.status.initialized || manager.status.boot_checked || reason == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    manager.status.boot_checked = true;
    manager.status.boot_passed = false;
    manager.status.last_error = reason;
    return ESP_OK;
}

esp_err_t roundie_ota_rollback(void)
{
    if (!manager.status.initialized || manager.status.phase != ROUNDIE_OTA_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    refresh();
    if (!manager.status.rollback_possible) {
        return ESP_ERR_OTA_ROLLBACK_FAILED;
    }
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t roundie_ota_boot_result(bool passed)
{
    if (!manager.status.initialized || manager.status.boot_checked) {
        return ESP_ERR_INVALID_STATE;
    }
    manager.status.boot_checked = true;
    manager.status.boot_passed = false;
    esp_err_t error = passed ? ESP_OK : ESP_FAIL;
    if (manager.status.running.state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (passed) {
            error = esp_ota_mark_app_valid_cancel_rollback();
        } else {
            error = roundie_ota_rollback();
        }
    } else if (manager.status.running.state != ESP_OTA_IMG_VALID &&
               manager.status.running.state != ESP_OTA_IMG_UNDEFINED &&
               manager.status.running.state != -2) {
        error = ESP_ERR_INVALID_STATE;
    }
    manager.status.boot_passed = passed && error == ESP_OK;
    manager.status.last_error = error;
    refresh();
    return error;
}

esp_err_t roundie_ota_begin(uint32_t session, size_t size, const uint8_t digest[32])
{
    if (!manager.status.initialized || !manager.status.boot_passed ||
        manager.status.phase != ROUNDIE_OTA_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    refresh();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (next == NULL || next != manager.target || next == manager.running ||
        boot != manager.running || manager.status.running.state != ESP_OTA_IMG_VALID ||
        !manager.status.running.image_valid || !manager.status.running.compatible) {
        return ESP_ERR_OTA_PARTITION_CONFLICT;
    }
    if (session == 0 || digest == NULL || size < PREFIX_SIZE || size > next->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    manager.status.received = 0;
    manager.status.total = size;
    manager.status.last_error = ESP_OK;
    manager.status.digest[0] = '\0';
    manager.status.session = session;
    memcpy(manager.expected, digest, 32);
    /* Sequential erasure bounds each write's Flash work, rather than erasing
     * the entire image in begin. IDF still invalidates inactive otadata here. */
    esp_err_t error = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &manager.handle);
    if (error != ESP_OK) {
        return fail(error);
    }
    manager.handle_open = true;
    manager.hash = psa_hash_operation_init();
    if (psa_hash_setup(&manager.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return fail(ESP_FAIL);
    }
    manager.status.phase = ROUNDIE_OTA_RECEIVING;
    esp_ota_img_states_t target_state;
    manager.status.other.state = esp_ota_get_state_partition(next, &target_state) == ESP_OK ?
        (int)target_state : -2;
    manager.status.other.image_valid = false;
    manager.status.other.compatible = false;
    manager.status.other.image_size = 0;
    manager.status.other.version[0] = '\0';
    manager.status.other.elf[0] = '\0';
    manager.status.rollback_possible = false;
    manager.last_activity_us = manager.now_us;
    return ESP_OK;
}

static bool descriptor_valid(const esp_app_desc_t *desc)
{
    return desc->magic_word == ESP_APP_DESC_MAGIC_WORD &&
           memchr(desc->version, '\0', sizeof(desc->version)) != NULL && desc->version[0] != '\0' &&
           memchr(desc->idf_ver, '\0', sizeof(desc->idf_ver)) != NULL &&
           memchr(desc->project_name, '\0', sizeof(desc->project_name)) != NULL &&
           strcmp(desc->project_name, "roundie_rep") == 0;
}

esp_err_t roundie_ota_write(uint32_t session, size_t offset, const void *data, size_t size)
{
    if (manager.status.phase != ROUNDIE_OTA_RECEIVING || session != manager.status.session) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || size == 0 || size > CHUNK_MAX || offset != manager.status.received ||
        size > manager.status.total - manager.status.received) {
        return fail(ESP_ERR_INVALID_SIZE);
    }
    if (offset == 0) {
        if (size < PREFIX_SIZE) {
            return fail(ESP_ERR_INVALID_SIZE);
        }
        esp_image_header_t header;
        esp_app_desc_t desc;
        memcpy(&header, data, sizeof(header));
        memcpy(&desc, (const uint8_t *)data + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(desc));
        if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
            header.segment_count > 16 || !descriptor_valid(&desc)) {
            return fail(ESP_ERR_OTA_VALIDATE_FAILED);
        }
        esp_err_t error = esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP, &header, &desc);
        if (error != ESP_OK) {
            return fail(error);
        }
    }
    esp_err_t error = esp_ota_write(manager.handle, data, size);
    if (error != ESP_OK) {
        return fail(error);
    }
    if (psa_hash_update(&manager.hash, data, size) != PSA_SUCCESS) {
        return fail(ESP_FAIL);
    }
    manager.status.received += size;
    manager.last_activity_us = manager.now_us;
    return ESP_OK;
}

esp_err_t roundie_ota_finish(uint32_t session)
{
    if (manager.status.phase != ROUNDIE_OTA_RECEIVING || session != manager.status.session) {
        return ESP_ERR_INVALID_STATE;
    }
    if (manager.status.received != manager.status.total) {
        return fail(ESP_ERR_INVALID_SIZE);
    }
    manager.status.phase = ROUNDIE_OTA_VERIFYING;
    uint8_t actual[32];
    size_t digest_size = 0;
    if (psa_hash_finish(&manager.hash, actual, sizeof(actual), &digest_size) != PSA_SUCCESS ||
        digest_size != sizeof(actual) ||
        memcmp(actual, manager.expected, sizeof(actual)) != 0) {
        return fail(ESP_ERR_INVALID_CRC);
    }
    psa_hash_abort(&manager.hash);
    /* end frees the IDF handle on BOTH success and failure. */
    manager.handle_open = false;
    esp_err_t error = esp_ota_end(manager.handle);
    if (error != ESP_OK) {
        return fail(error);
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(manager.target, &desc) != ESP_OK ||
        !descriptor_valid(&desc)) {
        return fail(ESP_ERR_OTA_VALIDATE_FAILED);
    }
    refresh();
    if (!manager.status.other.image_valid || manager.status.other.image_size != manager.status.total) {
        return fail(ESP_ERR_OTA_VALIDATE_FAILED);
    }
    hex_digest(actual, manager.status.digest);
    manager.status.phase = ROUNDIE_OTA_READY;
    manager.last_activity_us = manager.now_us;
    return ESP_OK;
}

esp_err_t roundie_ota_abort(uint32_t session)
{
    if ((manager.status.phase != ROUNDIE_OTA_RECEIVING && manager.status.phase != ROUNDIE_OTA_READY) ||
        session != manager.status.session) {
        return ESP_ERR_INVALID_STATE;
    }
    return fail(ESP_OK);
}

esp_err_t roundie_ota_activate(const uint8_t digest[32])
{
    if (manager.status.phase != ROUNDIE_OTA_READY || digest == NULL ||
        memcmp(digest, manager.expected, 32) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_ota_set_boot_partition(manager.target);
    manager.status.last_error = error;
    /* A failed metadata write has uncertain persistent state. Block all new
     * sessions until reboot even on failure; never retry by erasing a slot. */
    manager.status.phase = ROUNDIE_OTA_ACTIVATION_PENDING;
    refresh();
    return error;
}

void roundie_ota_tick(int64_t now_us)
{
    manager.now_us = now_us;
    if ((manager.status.phase == ROUNDIE_OTA_RECEIVING &&
         now_us - manager.last_activity_us >= SESSION_TIMEOUT_US) ||
        (manager.status.phase == ROUNDIE_OTA_READY &&
         now_us - manager.last_activity_us >= READY_TIMEOUT_US)) {
        fail(ESP_ERR_TIMEOUT);
    }
}

const char *roundie_ota_phase_name(roundie_ota_phase_t phase)
{
    static const char *names[] = {"IDLE", "RECEIVING", "VERIFYING", "READY", "ACTIVATION_PENDING"};
    return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "UNKNOWN";
}

const char *roundie_ota_image_state_name(int state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID: return "VALID";
    case ESP_OTA_IMG_INVALID: return "INVALID";
    case ESP_OTA_IMG_ABORTED: return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED: return "UNDEFINED";
    default: return "NO_METADATA";
    }
}
