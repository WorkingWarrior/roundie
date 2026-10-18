/* Actual manager/parser with modeled IDF Flash/metadata, real host SHA-256.
 * This does not emulate the bootloader or establish physical rollback. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ota_mock.h"
#include "roundie_ota.h"
#include "roundie_ota_protocol.h"

static esp_partition_t a = {0, 16, 0x10000, 0x380000, "ota_0"};
static esp_partition_t b = {0, 17, 0x390000, 0x380000, "ota_1"};
static esp_partition_t data = {1, 0, 0xd000, 0x2000, "otadata"};
static const esp_partition_t *running = &a;
static const esp_partition_t *boot = &a;
static const esp_partition_t *next = &b;
static esp_ota_img_states_t states[2];
static bool valid[2];
static bool open_handle;
static unsigned aborts, ends, selections, confirmations, rejections, writes;
static bool begin_error, write_error, end_error, select_error, confirm_error;
static uint8_t firmware[1024], flashed[1024], digest[32];
static size_t written;
static size_t verified_size;

static int slot(const esp_partition_t *p) {return p == &a ? 0 : 1;}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    (void)label;
    if (type == 1) {return &data;}
    return subtype == 16 ? &a : &b;
}
const esp_partition_t *esp_ota_get_running_partition(void) {return running;}
const esp_partition_t *esp_ota_get_boot_partition(void) {return boot;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start) {(void)start; return next;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state)
{
    *state = states[slot(p)];
    return ESP_OK;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc)
{
    (void)p;
    memcpy(desc, firmware + 32, sizeof(*desc));
    return ESP_OK;
}
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *p, esp_image_metadata_t *metadata)
{
    (void)mode;
    metadata->image_len = verified_size;
    return valid[p->offset == a.address ? 0 : 1] ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_ota_check_image_validity(esp_partition_type_t type, const esp_image_header_t *header, const esp_app_desc_t *desc)
{
    (void)type;
    (void)desc;
    return header->chip_id == 9 ? ESP_OK : ESP_FAIL;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *handle)
{
    assert(p != running && size == OTA_WITH_SEQUENTIAL_WRITES && !open_handle);
    if (begin_error) {return ESP_FAIL;}
    open_handle = true;
    *handle = 42;
    written = 0;
    states[slot(p)] = ESP_OTA_IMG_UNDEFINED;
    valid[slot(p)] = false;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *bytes, size_t size)
{
    assert(handle == 42 && open_handle && written + size <= sizeof(flashed));
    if (write_error) {return ESP_FAIL;}
    memcpy(flashed + written, bytes, size);
    written += size;
    writes++;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 42 && open_handle);
    open_handle = false;
    ends++;
    valid[slot(next)] = !end_error;
    return end_error ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == 42 && open_handle);
    open_handle = false;
    aborts++;
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    selections++;
    if (select_error) {return ESP_FAIL;}
    boot = p;
    states[slot(p)] = ESP_OTA_IMG_NEW;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    confirmations++;
    if (confirm_error) {return ESP_FAIL;}
    states[slot(running)] = ESP_OTA_IMG_VALID;
    return ESP_OK;
}
bool esp_ota_check_rollback_is_possible(void)
{
    int other = 1 - slot(running);
    return valid[other] && states[other] == ESP_OTA_IMG_VALID;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    assert(esp_ota_check_rollback_is_possible());
    rejections++;
    return ESP_FAIL; /* Reboot cannot complete in a host test. */
}

static roundie_ota_status_t status(void)
{
    roundie_ota_status_t out;
    roundie_ota_status(&out);
    return out;
}

static void reset(void)
{
    assert(!open_handle);
    running = &a;
    boot = &a;
    next = &b;
    states[0] = ESP_OTA_IMG_VALID;
    states[1] = ESP_OTA_IMG_UNDEFINED;
    valid[0] = true;
    valid[1] = false;
    begin_error = write_error = end_error = select_error = confirm_error = false;
    aborts = ends = selections = confirmations = rejections = writes = 0;
    verified_size = sizeof(firmware);
    memset(firmware, 0, sizeof(firmware));
    esp_image_header_t header = {.magic = 0xe9, .segment_count = 3, .chip_id = 9};
    esp_app_desc_t desc = {.magic_word = ESP_APP_DESC_MAGIC_WORD, .version = "0.2.1", .project_name = "roundie_rep", .idf_ver = "v6.1"};
    memcpy(firmware, &header, sizeof(header));
    memcpy(firmware + 32, &desc, sizeof(desc));
    unsigned length;
    assert(EVP_Digest(firmware, sizeof(firmware), digest, &length, EVP_sha256(), NULL) == 1);
    assert(length == 32);
    assert(roundie_ota_init() == ESP_OK);
    assert(roundie_ota_boot_result(true) == ESP_OK);
}

static void receive(void)
{
    assert(roundie_ota_begin(7, sizeof(firmware), digest) == ESP_OK);
    assert(roundie_ota_write(7, 0, firmware, 512) == ESP_OK);
    assert(roundie_ota_write(7, 512, firmware + 512, 512) == ESP_OK);
}

static void parser_tests(void)
{
    roundie_ota_command_t command;
    const char *bad[] = {"", "!RO2 1234abcd STATUS", "!RO1 1234ABCD STATUS",
        "!RO1 1234abcd STATUS ", "!RO1 1234abcd  STATUS", "!RO1 1234abcd BEGIN 00000000 512 ff",
        "!RO1 1234abcd DATA 1234abcd -1 00", "!RO1 1234abcd DATA 1234abcd 0 f",
        "!RO1 1234abcd DATA 1234abcd 0 gg", "!RO1 1234abcd DATA 1234abcd 3670017 00",
        "!RO1 1234abcd DATA 1234abcd 99999999999999999 00", "!RO1 1234abcd END 1234abcd extra",
        "!RO1 1234abcd ROLLBACK", "!RO1 1234abcd STATUS\r"};
    char line[ROUNDIE_OTA_LINE_CAPACITY + 1];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        strcpy(line, bad[i]);
        assert(roundie_ota_parse(line, false, &command) != ESP_OK);
    }
    strcpy(line, "!RO1 1234abcd STATUS");
    assert(roundie_ota_parse(line, false, &command) == ESP_OK);
    assert(command.operation == ROUNDIE_OTA_STATUS && strcmp(command.id, "1234abcd") == 0);
    strcpy(line, "!RO1 1234abcd STATUS");
    assert(roundie_ota_parse(line, true, &command) != ESP_OK);
    strcpy(line, "!RO1 1234abcd DATA 76543210 512 00ff");
    assert(roundie_ota_parse(line, false, &command) == ESP_OK);
    assert(command.session == 0x76543210 && command.offset == 512 && command.size == 2 && command.data[1] == 255);
    const char *prefix = "!RO1 1234abcd DATA 76543210 0 ";
    size_t prefix_length = strlen(prefix);
    strcpy(line, prefix);
    memset(line + prefix_length, 'a', 1024);
    line[prefix_length + 1024] = '\0';
    assert(roundie_ota_parse(line, false, &command) == ESP_OK && command.size == 512);
    strcpy(line, prefix);
    memset(line + prefix_length, 'a', 1026);
    line[prefix_length + 1026] = '\0';
    assert(roundie_ota_parse(line, false, &command) != ESP_OK);
}

int main(void)
{
    parser_tests();
    reset();
    assert(roundie_ota_activate(digest) != ESP_OK);
    assert(roundie_ota_begin(7, 0, digest) != ESP_OK);
    assert(roundie_ota_begin(7, 0x380001, digest) != ESP_OK);
    receive();
    assert(roundie_ota_begin(8, 1024, digest) != ESP_OK);
    assert(roundie_ota_abort(8) != ESP_OK && open_handle);
    assert(roundie_ota_finish(7) == ESP_OK && !open_handle && status().phase == ROUNDIE_OTA_READY);
    assert(memcmp(firmware, flashed, 1024) == 0 && selections == 0);
    uint8_t wrong[32] = {0};
    assert(roundie_ota_activate(wrong) != ESP_OK && selections == 0);
    assert(roundie_ota_activate(digest) == ESP_OK && boot == &b);
    assert(roundie_ota_begin(8, 1024, digest) != ESP_OK);
    assert(roundie_ota_abort(7) != ESP_OK);

    reset();
    for (int i = 0; i < 50; i++) {
        assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
        assert(roundie_ota_abort(7) == ESP_OK && !open_handle);
    }
    assert(aborts == 50 && selections == 0);
    assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
    roundie_ota_tick(15000000);
    assert(!open_handle && status().last_error == ESP_ERR_TIMEOUT);

    reset();
    assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
    assert(roundie_ota_write(8, 0, firmware, 512) != ESP_OK && open_handle);
    assert(roundie_ota_write(7, 1, firmware, 512) != ESP_OK && !open_handle && writes == 0);
    receive();
    assert(roundie_ota_abort(7) == ESP_OK);
    assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
    assert(roundie_ota_finish(7) != ESP_OK && !open_handle && selections == 0);

    reset();
    assert(roundie_ota_begin(7, 1024, wrong) == ESP_OK);
    assert(roundie_ota_write(7, 0, firmware, 512) == ESP_OK);
    assert(roundie_ota_write(7, 512, firmware + 512, 512) == ESP_OK);
    assert(roundie_ota_finish(7) == ESP_ERR_INVALID_CRC && ends == 0 && !open_handle);

    reset();
    receive();
    assert(roundie_ota_finish(7) == ESP_OK);
    roundie_ota_tick(300000000);
    assert(status().phase == ROUNDIE_OTA_IDLE && status().last_error == ESP_ERR_TIMEOUT);
    assert(selections == 0 && !open_handle);
    reset();
    receive();
    end_error = true;
    assert(roundie_ota_finish(7) != ESP_OK && ends == 1 && aborts == 0 && !open_handle);
    reset();
    receive();
    verified_size--;
    assert(roundie_ota_finish(7) != ESP_OK && selections == 0);
    reset();
    receive();
    assert(roundie_ota_finish(7) == ESP_OK);
    select_error = true;
    assert(roundie_ota_activate(digest) != ESP_OK && status().phase == ROUNDIE_OTA_ACTIVATION_PENDING);
    assert(roundie_ota_begin(9, 1024, digest) != ESP_OK);

    reset();
    begin_error = true;
    assert(roundie_ota_begin(7, 1024, digest) != ESP_OK && !open_handle);
    reset();
    assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
    write_error = true;
    assert(roundie_ota_write(7, 0, firmware, 512) != ESP_OK && !open_handle);
    reset();
    assert(roundie_ota_begin(7, 1024, digest) == ESP_OK);
    firmware[12] = 0; /* wrong target chip */
    assert(roundie_ota_write(7, 0, firmware, 512) != ESP_OK && writes == 0 && !open_handle);

    reset();
    next = &a;
    assert(roundie_ota_begin(7, 1024, digest) == ESP_ERR_OTA_PARTITION_CONFLICT);
    next = &b;
    boot = &b;
    assert(roundie_ota_begin(7, 1024, digest) == ESP_ERR_OTA_PARTITION_CONFLICT);
    b.address++;
    assert(roundie_ota_init() != ESP_OK);
    b.address--;

    reset();
    states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    assert(roundie_ota_init() == ESP_OK);
    assert(roundie_ota_begin(7, 1024, digest) != ESP_OK);
    assert(roundie_ota_boot_result(true) == ESP_OK && confirmations == 1);
    assert(roundie_ota_boot_result(true) != ESP_OK && confirmations == 1);
    assert(status().running.state == ESP_OTA_IMG_VALID);
    reset();
    states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    assert(roundie_ota_init() == ESP_OK);
    assert(!roundie_ota_test_reset_allowed());
    assert(roundie_ota_boot_result(false) == ESP_ERR_OTA_ROLLBACK_FAILED && rejections == 0 && confirmations == 0);
    assert(!status().boot_passed && status().running.state == ESP_OTA_IMG_PENDING_VERIFY);
    reset();
    states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    states[1] = ESP_OTA_IMG_VALID;
    valid[1] = true;
    assert(roundie_ota_init() == ESP_OK && roundie_ota_test_reset_allowed());
    assert(roundie_ota_boot_result(false) != ESP_OK && rejections == 1 && confirmations == 0);
    reset();
    states[0] = ESP_OTA_IMG_PENDING_VERIFY;
    confirm_error = true;
    assert(roundie_ota_init() == ESP_OK);
    assert(roundie_ota_boot_result(true) != ESP_OK && !status().boot_passed);
    reset();
    states[0] = ESP_OTA_IMG_UNDEFINED;
    assert(roundie_ota_init() == ESP_OK);
    assert(roundie_ota_boot_result(true) == ESP_OK && confirmations == 0);
    assert(roundie_ota_begin(7, 1024, digest) != ESP_OK);
    reset();
    assert(roundie_ota_rollback() == ESP_ERR_OTA_ROLLBACK_FAILED && rejections == 0);
    const esp_ota_img_states_t ineligible[] = {ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED};
    for (size_t i = 0; i < sizeof(ineligible) / sizeof(ineligible[0]); i++) {
        reset();
        valid[1] = true;
        states[1] = ineligible[i];
        assert(roundie_ota_init() == ESP_OK && !roundie_ota_test_reset_allowed());
        assert(roundie_ota_rollback() == ESP_ERR_OTA_ROLLBACK_FAILED && rejections == 0);
    }
    reset();
    running = &b;
    next = &a;
    boot = &b;
    valid[1] = true;
    states[1] = ESP_OTA_IMG_VALID;
    assert(roundie_ota_init() == ESP_OK);
    assert(roundie_ota_boot_result(true) == ESP_OK);
    assert(roundie_ota_begin(9, 1024, digest) == ESP_OK);
    assert(roundie_ota_abort(9) == ESP_OK);
    puts("OTA manager/parser: lifecycle, digest, metadata, failure cleanup and confirmation scenarios PASS (mock IDF)");
    return 0;
}
