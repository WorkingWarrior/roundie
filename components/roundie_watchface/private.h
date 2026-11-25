#pragma once
/* Internal to roundie_watchface: the unit the system worker hands to the GUI
 * task, and the two firmware backends. Not an ABI. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "roundie_face_format.h"
#include "roundie_face_text.h"
#include "rwf/rwf_face_v1.h"
#include "roundie_watchface.h"

/* Declarative backend instance; GUI task only. */
typedef struct {
    const roundie_face_definition_t *definition;
    const roundie_face_package_t *package;
    const uint8_t *bytes;
    uint8_t *settings; /* definition->setting_count values, owned by the loaded face */
    const rwf_host_v1_t *host;
    lv_obj_t *root;
    lv_obj_t *objects[ROUNDIE_FACE_ELEMENTS_MAX];
    lv_image_dsc_t images[ROUNDIE_FACE_RESOURCES_MAX];
    uint16_t angles[ROUNDIE_FACE_ELEMENTS_MAX]; /* drawn hand angle, tenths of a degree */
    rwf_data_v1_t data;
    bool created;
} roundie_face_declarative_t;

/* Worker -> GUI: a fully validated face. Ownership moves with the pointer;
 * the GUI frees it with roundie_face_loaded_free() after destroy. */
typedef struct {
    uint32_t serial;
    uint32_t purpose; /* roundie_face_purpose_t */
    char id[ROUNDIE_FACE_ID];
    roundie_face_package_t package;
    roundie_face_definition_t definition;
    uint8_t settings[ROUNDIE_FACE_SETTINGS_MAX];
    uint8_t *bytes; /* the whole verified package; resources point into it */
    size_t size;
    rwf_face_v1_t vtable; /* declarative entries, this face's needs and requirements */
    roundie_face_declarative_t instance;
} roundie_face_loaded_t;

#define ROUNDIE_FACE_HOST_CAPS (RWF_HOST_CAP_RESOURCES | RWF_HOST_CAP_SETTINGS | RWF_HOST_CAP_BATTERY)

typedef enum {
    ROUNDIE_FACE_PURPOSE_BOOT = 1, /* the stored preference at start */
    ROUNDIE_FACE_PURPOSE_SELECT,   /* explicit selection: persisted after a successful activation */
    ROUNDIE_FACE_PURPOSE_PREVIEW,  /* gallery: shown, not persisted */
    ROUNDIE_FACE_PURPOSE_REMOVED,  /* the active face was removed: fallback */
} roundie_face_purpose_t;

void roundie_face_declarative_bind(roundie_face_loaded_t *loaded);
/* Frees a face the GUI no longer uses (after destroy). NULL is ignored. */
void roundie_face_loaded_free(roundie_face_loaded_t *loaded);

/* The built-in fallback face: firmware only, no package, no storage. */
extern const rwf_face_v1_t roundie_face_fallback;
extern void *const roundie_face_fallback_instance;

/* Mailbox between the store (worker) and the GUI (store.c). Every call
 * holds the lock for a copy only. take() returns true when an offer was
 * due; *face NULL then means the fallback. Ownership of *face moves to the
 * caller. */
bool roundie_face_mailbox_take(roundie_face_loaded_t **face, uint32_t *purpose, uint32_t *serial);
void roundie_face_mailbox_report(uint32_t serial, uint32_t purpose, bool success, const char *id);
void roundie_face_mailbox_preview(const char *id);
void roundie_face_mailbox_persist(const char *id);
void roundie_face_mailbox_settings(const char *id, const uint8_t *values, unsigned count);
/* GUI-owned status fields, copied in under the lock. */
void roundie_face_mailbox_gui_status(const roundie_watchface_status_t *gui);
