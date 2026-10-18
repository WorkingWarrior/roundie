/* Compile the exact patched upstream helper in a minimal host environment. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef int32_t lfs_ssize_t;
typedef struct { uint32_t block_count; } lfs_t;
typedef struct { struct { uint32_t block_size; } cfg; lfs_t *fs; } esp_littlefs_t;
static int depth;
static lfs_ssize_t traversal_result;
static void sem_take(esp_littlefs_t *efs) { (void)efs; assert(depth++ == 0); }
static void sem_give(esp_littlefs_t *efs) { (void)efs; assert(--depth == 0); }
static lfs_ssize_t lfs_fs_size(lfs_t *fs) { (void)fs; return traversal_result; }
#include "capacity_helper.inc"

int main(void)
{
    lfs_t fs = {.block_count = 100};
    esp_littlefs_t efs = {.cfg = {.block_size = 4096}, .fs = &fs};
    size_t total = 1;
    size_t used = 1;
    traversal_result = -5; /* LFS_ERR_IO */
    assert(get_total_and_used_bytes(&efs, &total, &used) == ESP_FAIL);
    assert(total == 0 && used == 0 && depth == 0);
    traversal_result = -84; /* LFS_ERR_CORRUPT */
    assert(get_total_and_used_bytes(&efs, &total, &used) == ESP_FAIL);
    traversal_result = 2;
    assert(get_total_and_used_bytes(&efs, &total, &used) == ESP_OK);
    assert(total == 409600 && used == 8192);
    traversal_result = 101; /* Documented COW overcount is still saturated. */
    assert(get_total_and_used_bytes(&efs, &total, &used) == ESP_OK);
    assert(total == used && depth == 0);
    puts("Pinned LittleFS capacity helper: PASS (I/O/corrupt errors and COW saturation)");
    return 0;
}
