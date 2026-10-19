#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <openssl/evp.h>

typedef int esp_err_t;
enum {ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG,
      ESP_ERR_INVALID_SIZE, ESP_ERR_NOT_SUPPORTED, ESP_ERR_NOT_FOUND,
      ESP_ERR_INVALID_CRC, ESP_ERR_TIMEOUT, ESP_ERR_OTA_ROLLBACK_FAILED,
      ESP_ERR_OTA_PARTITION_CONFLICT, ESP_ERR_OTA_VALIDATE_FAILED};
enum {ESP_PARTITION_TYPE_APP, ESP_PARTITION_TYPE_DATA};
enum {ESP_PARTITION_SUBTYPE_APP_OTA_0 = 16, ESP_PARTITION_SUBTYPE_APP_OTA_1,
      ESP_PARTITION_SUBTYPE_DATA_OTA = 0};
typedef int esp_partition_type_t;
typedef struct {
    int type;
    unsigned subtype;
    uint32_t address;
    uint32_t size;
    char label[17];
} esp_partition_t;
typedef enum {ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
              ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1} esp_ota_img_states_t;
typedef unsigned esp_ota_handle_t;
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-2)
#define ESP_IMAGE_VERIFY_SILENT 2
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
typedef struct {uint32_t offset; uint32_t size;} esp_partition_pos_t;
typedef struct {uint32_t image_len;} esp_image_metadata_t;
typedef struct {uint8_t magic; uint8_t segment_count; uint8_t reserved[10]; uint16_t chip_id; uint8_t tail[10];} esp_image_header_t;
typedef struct {uint32_t load_addr; uint32_t data_len;} esp_image_segment_header_t;
typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint8_t tail[80];
} esp_app_desc_t;
_Static_assert(sizeof(esp_image_header_t) == 24, "mock header size");
_Static_assert(sizeof(esp_app_desc_t) == 256, "mock descriptor size");
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc);
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *p, esp_image_metadata_t *metadata);
esp_err_t esp_ota_check_image_validity(esp_partition_type_t type, const esp_image_header_t *header, const esp_app_desc_t *desc);
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
bool esp_ota_check_rollback_is_possible(void);

typedef struct {EVP_MD_CTX *ctx;} psa_hash_operation_t;
#define PSA_ALG_SHA_256 1
#define PSA_SUCCESS 0
static inline int psa_hash_compute(int algorithm, const void *input, size_t size,
                                   uint8_t *output, size_t capacity, size_t *out_size)
{
    (void)algorithm;
    unsigned length = 0;
    if (capacity < 32 || EVP_Digest(input, size, output, &length, EVP_sha256(), NULL) != 1) {
        return -1;
    }
    *out_size = length;
    return 0;
}
static inline psa_hash_operation_t psa_hash_operation_init(void) {return (psa_hash_operation_t){0};}
static inline int psa_hash_setup(psa_hash_operation_t *op, int algorithm)
{
    (void)algorithm;
    op->ctx = EVP_MD_CTX_new();
    return op->ctx != NULL && EVP_DigestInit_ex(op->ctx, EVP_sha256(), NULL) == 1 ? 0 : -1;
}
static inline int psa_hash_update(psa_hash_operation_t *op, const void *data, size_t size)
{
    return EVP_DigestUpdate(op->ctx, data, size) == 1 ? 0 : -1;
}
static inline int psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t capacity, size_t *size)
{
    unsigned length;
    if (capacity < 32 || EVP_DigestFinal_ex(op->ctx, out, &length) != 1) {
        return -1;
    }
    *size = length;
    return 0;
}
static inline int psa_hash_abort(psa_hash_operation_t *op)
{
    EVP_MD_CTX_free(op->ctx);
    op->ctx = NULL;
    return 0;
}
