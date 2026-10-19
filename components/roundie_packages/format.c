#include "roundie_package_format.h"
#include <stdio.h>
#include <string.h>

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool zero(const uint8_t *p, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool text(const uint8_t *p, size_t capacity, char *out)
{
    const uint8_t *end = memchr(p, 0, capacity);
    if (end == NULL || end == p || !zero(end, capacity - (size_t)(end - p))) {
        return false;
    }
    /* Canonical UTF-8, excluding controls, surrogates and overlong encodings. */
    for (const uint8_t *at = p; at < end;) {
        uint32_t value = *at++;
        unsigned count = 0;
        uint32_t minimum = 0;
        if (value >= 0xc2 && value <= 0xdf) {
            value &= 0x1f;
            count = 1;
            minimum = 0x80;
        } else if (value >= 0xe0 && value <= 0xef) {
            value &= 0x0f;
            count = 2;
            minimum = 0x800;
        } else if (value >= 0xf0 && value <= 0xf4) {
            value &= 7;
            count = 3;
            minimum = 0x10000;
        } else if (value >= 0x80) {
            return false;
        }
        if ((size_t)(end - at) < count) {
            return false;
        }
        for (unsigned i = 0; i < count; i++) {
            if ((*at & 0xc0) != 0x80) {
                return false;
            }
            value = value << 6 | (*at++ & 0x3f);
        }
        if (value < minimum || value < 0x20 || (value >= 0x7f && value <= 0x9f) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
    }
    memcpy(out, p, capacity);
    return true;
}

bool roundie_package_id_valid(const char *id)
{
    size_t length = strnlen(id, ROUNDIE_PACKAGE_ID);
    if (length < 3 || length >= ROUNDIE_PACKAGE_ID) {
        return false;
    }
    bool segment_start = true;
    bool dot = false;
    for (size_t i = 0; i < length; i++) {
        char ch = id[i];
        if (ch == '.') {
            if (segment_start) {
                return false;
            }
            segment_start = true;
            dot = true;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (!segment_start && ch >= '0' && ch <= '9'))) {
                return false;
            }
            segment_start = false;
        }
    }
    return dot && !segment_start;
}

bool roundie_package_decode(const void *prefix, size_t available, size_t total,
                            roundie_package_metadata_t *out,
                            char error[ROUNDIE_ELF_ERROR_SIZE])
{
    const uint8_t *p = prefix;
    *out = (roundie_package_metadata_t){0};
    const char *reason = "package-header";
    if (available < ROUNDIE_PACKAGE_PREFIX || total < ROUNDIE_PACKAGE_PREFIX + 52 ||
        total > ROUNDIE_PACKAGE_MAX || memcmp(p, "RPK1", 4) != 0 ||
        read16(p + 4) != 1 || read16(p + 6) != ROUNDIE_PACKAGE_HEADER ||
        read32(p + 8) != total || read32(p + 12) != ROUNDIE_PACKAGE_MANIFEST ||
        read32(p + 16) != total - ROUNDIE_PACKAGE_PREFIX ||
        !zero(p + 20, 4) || !zero(p + 56, 8)) {
        goto failed;
    }
    const uint8_t *manifest = p + ROUNDIE_PACKAGE_HEADER;
    reason = "package-manifest";
    if (!text(manifest, 64, out->id) || !roundie_package_id_valid(out->id) ||
        !text(manifest + 64, 64, out->name) || !text(manifest + 136, 16, out->target) ||
        !zero(manifest + 154, 6)) {
        goto failed;
    }
    for (unsigned i = 0; i < 3; i++) {
        out->version[i] = read16(manifest + 128 + i * 2);
    }
    out->abi = read16(manifest + 134);
    out->api = read16(manifest + 152);
    if (out->abi == 0 || out->api == 0) {
        goto failed;
    }
    out->elf_size = read32(p + 16);
    memcpy(out->elf_digest, p + 24, 32);
    error[0] = '\0';
    return true;
failed:
    snprintf(error, ROUNDIE_ELF_ERROR_SIZE, "%s", reason);
    return false;
}

bool roundie_package_compatible(const roundie_package_metadata_t *metadata)
{
    return metadata->abi >= 1 && metadata->abi <= 3 && metadata->api == ROUNDIE_PACKAGE_API &&
        strcmp(metadata->target, ROUNDIE_PACKAGE_TARGET) == 0;
}

int roundie_package_version_compare(const uint16_t left[3], const uint16_t right[3])
{
    for (unsigned i = 0; i < 3; i++) {
        if (left[i] != right[i]) {
            return left[i] > right[i] ? 1 : -1;
        }
    }
    return 0;
}
