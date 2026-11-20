#pragma once
/* Watch face package (.rwf) and declarative definition (docs/watch-face-packages.md).
 * Pure C: decodes serialized little-endian bytes, never a native struct cast.
 * The digest is checked by the store (PSA SHA-256), not here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUNDIE_FACE_HEADER 64U
#define ROUNDIE_FACE_MANIFEST 96U
#define ROUNDIE_FACE_SECTIONS 16U
#define ROUNDIE_FACE_PREFIX (ROUNDIE_FACE_HEADER + ROUNDIE_FACE_MANIFEST + ROUNDIE_FACE_SECTIONS)
#define ROUNDIE_FACE_RESOURCE_ENTRY 16U
#define ROUNDIE_FACE_PACKAGE_MAX 65536U
#define ROUNDIE_FACE_ID 48U
#define ROUNDIE_FACE_NAME 32U
#define ROUNDIE_FACE_ERROR 48U

#define ROUNDIE_FACE_BACKEND_DECLARATIVE 1U
#define ROUNDIE_FACE_BACKEND_NATIVE 2U /* reserved: refused in this release */

/* Definition limits (bounded parser). */
#define ROUNDIE_FACE_DEFINITION_MAX 4096U
#define ROUNDIE_FACE_ELEMENTS_MAX 32U
#define ROUNDIE_FACE_SETTINGS_MAX 4U
#define ROUNDIE_FACE_OPTIONS_MAX 8U
#define ROUNDIE_FACE_STRINGS_MAX 1024U
#define ROUNDIE_FACE_STRING_MAX 63U /* bytes, without the NUL */
#define ROUNDIE_FACE_DEPTH_MAX 4U
#define ROUNDIE_FACE_RESOURCES_MAX 8U
#define ROUNDIE_FACE_RESOURCE_BYTES_MAX 49152U
#define ROUNDIE_FACE_DEFINITION_HEADER 16U
#define ROUNDIE_FACE_SETTING_ENTRY 20U
#define ROUNDIE_FACE_ELEMENT_ENTRY 32U
#define ROUNDIE_FACE_NO_PARENT 0xFFU
#define ROUNDIE_FACE_NO_TEXT 0xFFFFU
#define ROUNDIE_FACE_NO_PREVIEW 0xFFU
#define ROUNDIE_FACE_RADIUS_CIRCLE 0xFFFFU

typedef enum {
    ROUNDIE_FACE_LABEL = 1,
    ROUNDIE_FACE_RECT = 2,  /* also a layer: the only element that may be a parent */
    ROUNDIE_FACE_ARC = 3,
    ROUNDIE_FACE_IMAGE = 4,
    ROUNDIE_FACE_HAND = 5,
    ROUNDIE_FACE_TICKS = 6,
} roundie_face_element_type_t;

typedef enum {
    ROUNDIE_FACE_HAND_HOUR = 0,
    ROUNDIE_FACE_HAND_MINUTE = 1,
    ROUNDIE_FACE_HAND_SECOND = 2,
} roundie_face_hand_t;

typedef enum {
    ROUNDIE_FACE_BIND_BATTERY = 1, /* 0..100 */
    ROUNDIE_FACE_BIND_SECOND = 2,  /* 0..59 */
    ROUNDIE_FACE_BIND_MINUTE = 3,  /* 0..59 */
    ROUNDIE_FACE_BIND_HOUR = 4,    /* 0..11 */
} roundie_face_binding_t;

typedef enum {
    ROUNDIE_FACE_WHEN_ALWAYS = 0,
    ROUNDIE_FACE_WHEN_TIME_VALID = 1,
    ROUNDIE_FACE_WHEN_TIME_INVALID = 2,
    ROUNDIE_FACE_WHEN_BATTERY_VALID = 3,
    ROUNDIE_FACE_WHEN_CHARGING = 4,
    ROUNDIE_FACE_WHEN_SETTING_IS = 5,
    ROUNDIE_FACE_WHEN_SETTING_NOT = 6,
    ROUNDIE_FACE_WHEN_NOT_CHARGING = 7,
} roundie_face_condition_t;

typedef enum {
    ROUNDIE_FACE_ACTION_NONE = 0,
    ROUNDIE_FACE_ACTION_CYCLE_SETTING = 1,
    ROUNDIE_FACE_ACTION_REFRESH = 2,
} roundie_face_action_t;

typedef struct {
    char id[ROUNDIE_FACE_ID];
    char name[ROUNDIE_FACE_NAME];
    uint16_t version[3];
    uint16_t abi_major;
    uint16_t abi_minor;
    uint8_t backend;
    uint8_t preview; /* resource index or ROUNDIE_FACE_NO_PREVIEW */
    uint32_t requires; /* RWF_HOST_CAP_* */
} roundie_face_manifest_t;

typedef struct {
    uint16_t format; /* RWF_RESOURCE_* */
    uint16_t width;
    uint16_t height;
    uint32_t offset; /* from the start of the package */
    uint32_t length;
} roundie_face_resource_t;

typedef struct {
    uint8_t type;   /* roundie_face_element_type_t */
    uint8_t parent; /* earlier RECT index or ROUNDIE_FACE_NO_PARENT */
    uint8_t variant; /* font, hand, tick count, binding or resource */
    uint8_t align;  /* LABEL: 0 left, 1 centre, 2 right */
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
    uint32_t color;
    uint32_t color2;
    uint16_t text; /* string offset, LABEL only */
    uint16_t size;
    uint8_t opacity;
    uint8_t condition;
    uint8_t condition_setting;
    uint8_t condition_value;
    uint8_t action;
    uint8_t action_setting;
    uint8_t extra; /* RECT border width, HAND tail, TICKS major length */
} roundie_face_element_t;

typedef struct {
    char key[16];
    uint8_t options;
    uint8_t initial;
} roundie_face_setting_t;

typedef struct {
    uint8_t element_count;
    uint8_t setting_count;
    uint16_t needs; /* RWF_NEED_* */
    uint32_t background;
    uint16_t string_bytes;
    roundie_face_setting_t settings[ROUNDIE_FACE_SETTINGS_MAX];
    roundie_face_element_t elements[ROUNDIE_FACE_ELEMENTS_MAX];
    char strings[ROUNDIE_FACE_STRINGS_MAX];
} roundie_face_definition_t;

typedef struct {
    roundie_face_manifest_t manifest;
    uint32_t total;
    uint32_t definition_offset;
    uint32_t definition_size;
    uint8_t resource_count;
    roundie_face_resource_t resources[ROUNDIE_FACE_RESOURCES_MAX];
    uint8_t digest[32]; /* as stored in the envelope */
} roundie_face_package_t;

/* Envelope, manifest, section and resource table, without the definition.
 * available may be the prefix only: then only the header is checked against
 * total (discovery). */
bool roundie_face_decode_package(const uint8_t *bytes, size_t available, size_t total,
                                 roundie_face_package_t *out, char error[ROUNDIE_FACE_ERROR]);
/* A declarative definition; every string, token, reference and limit. */
bool roundie_face_decode_definition(const uint8_t *bytes, size_t size, const roundie_face_package_t *package,
                                    roundie_face_definition_t *out, char error[ROUNDIE_FACE_ERROR]);
/* Major must match, minor at most the host's, required caps available. */
bool roundie_face_abi_compatible(uint16_t major, uint16_t minor, uint32_t requires, uint32_t host_caps);
bool roundie_face_id_valid(const char *id);
const char *roundie_face_text(const roundie_face_definition_t *definition, uint16_t offset);
