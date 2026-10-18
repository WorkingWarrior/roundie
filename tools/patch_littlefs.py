#!/usr/bin/env python3
"""Pinned 1.22.2 capacity-error fix, applied only to a build-directory copy."""

import hashlib
from pathlib import Path
import sys

SOURCE_SHA256 = "c64f9c206cc7de1df3c8ad62f9c9dedf0da61edb033f916e4d26d268709d4926"
OLD_BODY = """    sem_take(efs);
    size_t total_bytes_local = efs->cfg.block_size * efs->fs->block_count;
    if(total_bytes) *total_bytes = total_bytes_local;

    /* lfs_fs_size may return a size larger than the actual filesystem size.
     * https://github.com/littlefs-project/littlefs/blob/9c7e232086f865cff0bb96fe753deb66431d91fd/lfs.h#L658
     */
    if(used_bytes) *used_bytes = MIN(total_bytes_local, efs->cfg.block_size * lfs_fs_size(efs->fs));
    sem_give(efs);
"""
NEW_BODY = """    sem_take(efs);
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (used_bytes != NULL) {
        *used_bytes = 0;
    }
    lfs_ssize_t blocks = used_bytes != NULL ? lfs_fs_size(efs->fs) : 0;
    if (blocks < 0) {
        sem_give(efs);
        return ESP_FAIL;
    }
    size_t total = efs->cfg.block_size * efs->fs->block_count;
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        /* Preserve upstream's documented saturation for shared COW blocks. */
        *used_bytes = MIN(total, (size_t)efs->cfg.block_size * (size_t)blocks);
    }
    sem_give(efs);
    return ESP_OK;
"""


def patched_source(data):
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("LittleFS 1.22.2 source changed; review capacity patch before building")
    source = data.decode()
    source = source.replace("void get_total_and_used_bytes(", "esp_err_t get_total_and_used_bytes(")
    if source.count(OLD_BODY) != 1:
        raise ValueError("LittleFS capacity patch body does not match")
    source = source.replace(OLD_BODY, NEW_BODY)
    old_return = "    get_total_and_used_bytes(_efs[index], total_bytes, used_bytes);\n\n    return ESP_OK;"
    if source.count(old_return) != 4:
        raise ValueError("LittleFS capacity callers changed")
    return source.replace(old_return,
                          "    return get_total_and_used_bytes(_efs[index], total_bytes, used_bytes);")


if __name__ == "__main__":
    Path(sys.argv[2]).write_text(patched_source(Path(sys.argv[1]).read_bytes()))
