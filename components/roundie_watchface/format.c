#include "roundie_face_format.h"
#include <stdio.h>
#include <string.h>
#include "roundie_face_text.h"
#include "rwf/rwf_face_v1.h"

static uint16_t read16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | bytes[1] << 8);
}

static uint32_t read32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool zero(const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool fail(char error[ROUNDIE_FACE_ERROR], const char *reason)
{
    snprintf(error, ROUNDIE_FACE_ERROR, "%s", reason);
    return false;
}

/* Strict UTF-8: no overlong forms, surrogates or code points above U+10FFFF,
 * and no control characters. */
static bool utf8_valid(const uint8_t *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        uint8_t lead = text[i];
        if (lead < 0x20 || lead == 0x7f) {
            return false;
        }
        if (lead < 0x80) {
            i++;
            continue;
        }
        size_t extra;
        uint32_t code;
        if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
            code = lead & 0x1fU;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2;
            code = lead & 0x0fU;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3;
            code = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= length) {
            return false;
        }
        for (size_t j = 1; j <= extra; j++) {
            if ((text[i + j] & 0xc0) != 0x80) {
                return false;
            }
            code = code << 6 | (text[i + j] & 0x3fU);
        }
        if ((extra == 2 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff))) ||
            (extra == 3 && (code < 0x10000 || code > 0x10ffff))) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

/* A NUL-terminated, NUL-padded UTF-8 field. */
static bool text_field(const uint8_t *bytes, size_t size, char *out)
{
    size_t length = strnlen((const char *)bytes, size);
    if (length == 0 || length >= size || !zero(bytes + length, size - length) || !utf8_valid(bytes, length)) {
        return false;
    }
    memcpy(out, bytes, length + 1);
    return true;
}

bool roundie_face_id_valid(const char *id)
{
    size_t length = strnlen(id, ROUNDIE_FACE_ID);
    if (length < 3 || length >= ROUNDIE_FACE_ID) {
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

bool roundie_face_abi_compatible(uint16_t major, uint16_t minor, uint32_t requires, uint32_t host_caps)
{
    return major == RWF_ABI_MAJOR && minor <= RWF_ABI_MINOR && (requires & ~host_caps) == 0;
}

bool roundie_face_decode_package(const uint8_t *bytes, size_t available, size_t total,
                                 roundie_face_package_t *out, char error[ROUNDIE_FACE_ERROR])
{
    memset(out, 0, sizeof(*out));
    if (available < ROUNDIE_FACE_PREFIX || total < ROUNDIE_FACE_PREFIX || total > ROUNDIE_FACE_PACKAGE_MAX ||
        available > total) {
        return fail(error, "package-size");
    }
    if (memcmp(bytes, "RWF1", 4) != 0 || read16(bytes + 4) != 1 || read16(bytes + 6) != ROUNDIE_FACE_HEADER ||
        read32(bytes + 8) != total || !zero(bytes + 12, 20)) {
        return fail(error, "package-envelope");
    }
    memcpy(out->digest, bytes + 32, 32);
    out->total = (uint32_t)total;
    const uint8_t *manifest = bytes + ROUNDIE_FACE_HEADER;
    roundie_face_manifest_t *meta = &out->manifest;
    if (!text_field(manifest, ROUNDIE_FACE_ID, meta->id) || !roundie_face_id_valid(meta->id)) {
        return fail(error, "manifest-id");
    }
    if (!text_field(manifest + 48, ROUNDIE_FACE_NAME, meta->name)) {
        return fail(error, "manifest-name");
    }
    for (unsigned i = 0; i < 3; i++) {
        meta->version[i] = read16(manifest + 80 + i * 2);
    }
    meta->abi_major = read16(manifest + 86);
    meta->abi_minor = read16(manifest + 88);
    meta->backend = manifest[90];
    meta->preview = manifest[91];
    meta->requires = read32(manifest + 92);
    if (meta->abi_major == 0) {
        return fail(error, "manifest-abi");
    }
    if (meta->backend != ROUNDIE_FACE_BACKEND_DECLARATIVE && meta->backend != ROUNDIE_FACE_BACKEND_NATIVE) {
        return fail(error, "manifest-backend");
    }
    const uint8_t *sections = manifest + ROUNDIE_FACE_MANIFEST;
    out->definition_offset = read32(sections);
    out->definition_size = read32(sections + 4);
    uint16_t count = read16(sections + 8);
    if (!zero(sections + 10, 6)) {
        return fail(error, "sections-reserved");
    }
    if (count > ROUNDIE_FACE_RESOURCES_MAX) {
        return fail(error, "resource-count");
    }
    out->resource_count = (uint8_t)count;
    size_t table_end = ROUNDIE_FACE_PREFIX + (size_t)count * ROUNDIE_FACE_RESOURCE_ENTRY;
    if (out->definition_offset != table_end || out->definition_size == 0 ||
        out->definition_size > ROUNDIE_FACE_DEFINITION_MAX || table_end + out->definition_size > total) {
        return fail(error, "definition-bounds");
    }
    if (meta->preview != ROUNDIE_FACE_NO_PREVIEW && meta->preview >= count) {
        return fail(error, "manifest-preview");
    }
    if (available < table_end) {
        /* Discovery read only the fixed prefix; the store reads it all before use. */
        return count == 0 ? true : fail(error, "package-truncated");
    }
    /* Resources follow the definition contiguously, in table order. */
    size_t next = table_end + out->definition_size;
    size_t resource_bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *entry = bytes + ROUNDIE_FACE_PREFIX + i * ROUNDIE_FACE_RESOURCE_ENTRY;
        roundie_face_resource_t *resource = &out->resources[i];
        resource->format = read16(entry);
        resource->width = read16(entry + 2);
        resource->height = read16(entry + 4);
        resource->offset = read32(entry + 8);
        resource->length = read32(entry + 12);
        unsigned depth = resource->format == RWF_RESOURCE_RGB565 ? 2U : resource->format == RWF_RESOURCE_A8 ? 1U : 0U;
        if (depth == 0 || read16(entry + 6) != 0 || resource->width == 0 || resource->height == 0 ||
            resource->width > 360 || resource->height > 360) {
            return fail(error, "resource-format");
        }
        if (resource->offset != next || resource->length != (uint32_t)resource->width * resource->height * depth ||
            resource->length > total - next) {
            return fail(error, "resource-bounds");
        }
        resource_bytes += resource->length;
        next += resource->length;
    }
    if (resource_bytes > ROUNDIE_FACE_RESOURCE_BYTES_MAX) {
        return fail(error, "resource-budget");
    }
    if (next != total) {
        return fail(error, "package-trailing-bytes");
    }
    return true;
}

const char *roundie_face_text(const roundie_face_definition_t *definition, uint16_t offset)
{
    if (offset == ROUNDIE_FACE_NO_TEXT || offset >= definition->string_bytes) {
        return "";
    }
    return definition->strings + offset;
}

static bool setting_key_valid(const uint8_t *key)
{
    size_t length = strnlen((const char *)key, 16);
    if (length == 0 || length >= 16 || !zero(key + length, 16 - length)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= '0' && key[i] <= '9') || key[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool element_valid(const roundie_face_definition_t *definition, unsigned index,
                          const roundie_face_package_t *package, char error[ROUNDIE_FACE_ERROR])
{
    const roundie_face_element_t *element = &definition->elements[index];
    if (element->parent != ROUNDIE_FACE_NO_PARENT) {
        if (element->parent >= index || definition->elements[element->parent].type != ROUNDIE_FACE_RECT) {
            return fail(error, "element-parent");
        }
        unsigned depth = 1;
        for (unsigned at = element->parent; definition->elements[at].parent != ROUNDIE_FACE_NO_PARENT;
             at = definition->elements[at].parent) {
            if (++depth >= ROUNDIE_FACE_DEPTH_MAX) {
                return fail(error, "element-depth");
            }
        }
    }
    if (element->x < -360 || element->x > 720 || element->y < -360 || element->y > 720 ||
        element->w > 360 || element->h > 360 || (element->color >> 24) != 0 || (element->color2 >> 24) != 0) {
        return fail(error, "element-geometry");
    }
    switch (element->condition) {
    case ROUNDIE_FACE_WHEN_SETTING_IS:
    case ROUNDIE_FACE_WHEN_SETTING_NOT:
        if (element->condition_setting >= definition->setting_count ||
            element->condition_value >= definition->settings[element->condition_setting].options) {
            return fail(error, "element-condition");
        }
        break;
    case ROUNDIE_FACE_WHEN_ALWAYS:
    case ROUNDIE_FACE_WHEN_TIME_VALID:
    case ROUNDIE_FACE_WHEN_TIME_INVALID:
    case ROUNDIE_FACE_WHEN_BATTERY_VALID:
    case ROUNDIE_FACE_WHEN_CHARGING:
    case ROUNDIE_FACE_WHEN_NOT_CHARGING:
        if (element->condition_setting != 0 || element->condition_value != 0) {
            return fail(error, "element-condition");
        }
        break;
    default:
        return fail(error, "element-condition");
    }
    if (element->action == ROUNDIE_FACE_ACTION_CYCLE_SETTING) {
        if (element->action_setting >= definition->setting_count) {
            return fail(error, "element-action");
        }
    } else if (element->action > ROUNDIE_FACE_ACTION_REFRESH || element->action_setting != 0) {
        return fail(error, "element-action");
    }
    bool text = element->text != ROUNDIE_FACE_NO_TEXT;
    switch (element->type) {
    case ROUNDIE_FACE_LABEL:
        if (element->variant > 3 || element->align > 2 || !text || element->size != 0 || element->extra != 0) {
            return fail(error, "label-fields");
        }
        if (element->text >= definition->string_bytes ||
            (element->text > 0 && definition->strings[element->text - 1] != '\0')) {
            return fail(error, "label-text-offset");
        }
        if (!roundie_face_template_valid(roundie_face_text(definition, element->text))) {
            return fail(error, "label-template");
        }
        return true;
    case ROUNDIE_FACE_RECT:
        if (element->variant != 0 || element->align != 0 || text || element->w == 0 || element->h == 0) {
            return fail(error, "rect-fields");
        }
        return true;
    case ROUNDIE_FACE_ARC:
        if (element->variant < ROUNDIE_FACE_BIND_BATTERY || element->variant > ROUNDIE_FACE_BIND_HOUR ||
            element->align != 0 || text || element->w == 0 || element->w != element->h || element->size == 0 ||
            element->size > element->w / 2 || element->extra != 0) {
            return fail(error, "arc-fields");
        }
        return true;
    case ROUNDIE_FACE_IMAGE:
        if (element->variant >= package->resource_count || element->align != 0 || text || element->size != 0 ||
            element->extra != 0) {
            return fail(error, "image-fields");
        }
        return true;
    case ROUNDIE_FACE_HAND:
        if (element->variant > ROUNDIE_FACE_HAND_SECOND || element->align != 0 || text || element->w == 0 ||
            element->w > 180 || element->h != 0 || element->size == 0 || element->size > 16 || element->extra > 60) {
            return fail(error, "hand-fields");
        }
        return true;
    case ROUNDIE_FACE_TICKS:
        if ((element->variant != 12 && element->variant != 60) || element->align != 0 || text ||
            element->w == 0 || element->w > 180 || element->h != 0 || element->size == 0 || element->size > element->w ||
            element->extra > element->w) {
            return fail(error, "ticks-fields");
        }
        return true;
    default:
        return fail(error, "element-type");
    }
}

bool roundie_face_decode_definition(const uint8_t *bytes, size_t size, const roundie_face_package_t *package,
                                    roundie_face_definition_t *out, char error[ROUNDIE_FACE_ERROR])
{
    memset(out, 0, sizeof(*out));
    if (size < ROUNDIE_FACE_DEFINITION_HEADER || size > ROUNDIE_FACE_DEFINITION_MAX) {
        return fail(error, "definition-size");
    }
    if (memcmp(bytes, "RWFD", 4) != 0 || read16(bytes + 4) != 1) {
        return fail(error, "definition-header");
    }
    unsigned elements = bytes[6];
    unsigned settings = bytes[7];
    unsigned strings = read16(bytes + 8);
    unsigned needs = read16(bytes + 10);
    uint32_t background = read32(bytes + 12);
    if (elements == 0 || elements > ROUNDIE_FACE_ELEMENTS_MAX || settings > ROUNDIE_FACE_SETTINGS_MAX ||
        strings > ROUNDIE_FACE_STRINGS_MAX || (needs & ~(RWF_NEED_SECOND | RWF_NEED_BATTERY | RWF_NEED_TAP)) != 0 ||
        (background >> 24) != 0) {
        return fail(error, "definition-limits");
    }
    size_t expected = ROUNDIE_FACE_DEFINITION_HEADER + settings * ROUNDIE_FACE_SETTING_ENTRY +
        elements * ROUNDIE_FACE_ELEMENT_ENTRY + strings;
    if (expected != size) {
        return fail(error, "definition-length");
    }
    out->element_count = (uint8_t)elements;
    out->setting_count = (uint8_t)settings;
    out->needs = (uint16_t)needs;
    out->background = background;
    out->string_bytes = (uint16_t)strings;
    const uint8_t *cursor = bytes + ROUNDIE_FACE_DEFINITION_HEADER;
    for (unsigned i = 0; i < settings; i++, cursor += ROUNDIE_FACE_SETTING_ENTRY) {
        roundie_face_setting_t *setting = &out->settings[i];
        if (!setting_key_valid(cursor) || cursor[16] < 2 || cursor[16] > ROUNDIE_FACE_OPTIONS_MAX ||
            cursor[17] >= cursor[16] || cursor[18] != 0 || cursor[19] != 0) {
            return fail(error, "setting-fields");
        }
        memcpy(setting->key, cursor, 16);
        setting->options = cursor[16];
        setting->initial = cursor[17];
        for (unsigned j = 0; j < i; j++) {
            if (strcmp(out->settings[j].key, setting->key) == 0) {
                return fail(error, "setting-duplicate");
            }
        }
    }
    const uint8_t *element_bytes = cursor;
    const uint8_t *table = element_bytes + elements * ROUNDIE_FACE_ELEMENT_ENTRY;
    /* String table: NUL-terminated UTF-8 strings of bounded length, nothing after the last NUL. */
    if (strings > 0) {
        memcpy(out->strings, table, strings);
        if (out->strings[strings - 1] != '\0') {
            return fail(error, "strings-unterminated");
        }
        size_t start = 0;
        for (size_t i = 0; i < strings; i++) {
            if (out->strings[i] == '\0') {
                size_t length = i - start;
                if (length > ROUNDIE_FACE_STRING_MAX || !utf8_valid((const uint8_t *)out->strings + start, length)) {
                    return fail(error, "strings-content");
                }
                start = i + 1;
            }
        }
    }
    bool second = false;
    bool battery = false;
    bool tap = false;
    for (unsigned i = 0; i < elements; i++) {
        const uint8_t *entry = element_bytes + i * ROUNDIE_FACE_ELEMENT_ENTRY;
        roundie_face_element_t *element = &out->elements[i];
        element->type = entry[0];
        element->parent = entry[1];
        element->variant = entry[2];
        element->align = entry[3];
        element->x = (int16_t)read16(entry + 4);
        element->y = (int16_t)read16(entry + 6);
        element->w = read16(entry + 8);
        element->h = read16(entry + 10);
        element->color = read32(entry + 12);
        element->color2 = read32(entry + 16);
        element->text = read16(entry + 20);
        element->size = read16(entry + 22);
        element->opacity = entry[24];
        element->condition = entry[25];
        element->condition_setting = entry[26];
        element->condition_value = entry[27];
        element->action = entry[28];
        element->action_setting = entry[29];
        element->extra = entry[30];
        if (entry[31] != 0) {
            return fail(error, "element-reserved");
        }
        if (!element_valid(out, i, package, error)) {
            return false;
        }
        const char *template = element->type == ROUNDIE_FACE_LABEL ? roundie_face_text(out, element->text) : "";
        second = second || (element->type == ROUNDIE_FACE_HAND && element->variant == ROUNDIE_FACE_HAND_SECOND) ||
            (element->type == ROUNDIE_FACE_ARC && element->variant == ROUNDIE_FACE_BIND_SECOND) ||
            roundie_face_template_uses(template, ROUNDIE_FACE_USES_SECOND);
        battery = battery || (element->type == ROUNDIE_FACE_ARC && element->variant == ROUNDIE_FACE_BIND_BATTERY) ||
            element->condition == ROUNDIE_FACE_WHEN_BATTERY_VALID || element->condition == ROUNDIE_FACE_WHEN_CHARGING ||
            element->condition == ROUNDIE_FACE_WHEN_NOT_CHARGING ||
            roundie_face_template_uses(template, ROUNDIE_FACE_USES_BATTERY);
        tap = tap || element->action != ROUNDIE_FACE_ACTION_NONE;
    }
    /* The declared needs must cover what the definition binds; a second hand
     * without RWF_NEED_SECOND would freeze. More than needed is refused too,
     * so the compiler and the device agree on the schedule. */
    unsigned derived = (second ? RWF_NEED_SECOND : 0U) | (battery ? RWF_NEED_BATTERY : 0U) | (tap ? RWF_NEED_TAP : 0U);
    if (derived != needs) {
        return fail(error, "definition-needs");
    }
    return true;
}
