#pragma once
/* WatchFaceHost lifecycle (docs/watch-face-platform.md). Pure C over the
 * Watch Face ABI v1 vtable: no LVGL, no clock, no storage. The GUI task is
 * the only caller; nothing here locks.
 *
 * Per face: EMPTY -> CREATED (create) -> ACTIVE (activate) -> CREATED
 * (deactivate, also for park) -> EMPTY (destroy). At most one face is ACTIVE.
 * During a switch the old face is CREATED while the new one is created and
 * activated; the old face is destroyed only after the new one is active, and
 * activated again if the new one fails. A face that fails any call is
 * deactivated, destroyed and never retried by the host; the built-in fallback
 * takes its place. No call reaches a face after destroy or while parked. */
#include <stdbool.h>
#include <stdint.h>
#include "rwf/rwf_face_v1.h"

#define ROUNDIE_FACE_HOST_ID 48U

typedef enum {
    ROUNDIE_FACE_EMPTY,
    ROUNDIE_FACE_CREATED,
    ROUNDIE_FACE_ACTIVE,
} roundie_face_state_t;

typedef struct {
    const rwf_face_v1_t *face;
    void *instance;
    roundie_face_state_t state;
    uint32_t needs;
    bool fallback;
    char id[ROUNDIE_FACE_HOST_ID];
} roundie_face_slot_t;

typedef struct {
    roundie_face_slot_t current;
    const rwf_face_v1_t *fallback_face;
    void *fallback_instance;
    rwf_host_v1_t services; /* handed to every create */
    rwf_surface_t *surface;
    bool parked;
    bool have_sample;
    rwf_data_v1_t last; /* the sample the face last saw */
    bool refresh_requested;
    uint32_t requested_needs; /* set_needs during a call, applied after it */
    bool needs_changed;
    /* Counters for diagnostics and tests. */
    uint32_t creates;
    uint32_t destroys;
    uint32_t activations;
    uint32_t deactivations;
    uint32_t updates;
    uint32_t failures;
    uint32_t switches;
    uint32_t refused; /* negotiation or state refusals */
    int32_t last_error;
    char failed_id[ROUNDIE_FACE_HOST_ID];
} roundie_face_host_t;

/* services.context must point at the host's owner; data/resource/setting
 * entries are the owner's. The host routes request_refresh and set_needs
 * itself (it replaces those two entries). */
void roundie_face_host_init(roundie_face_host_t *host, const rwf_host_v1_t *services,
                            const rwf_face_v1_t *fallback_face, void *fallback_instance);
/* Negotiation without calling the face: size, ABI, capabilities, entries. */
int32_t roundie_face_host_check(const roundie_face_host_t *host, const rwf_face_v1_t *face);
/* Creates face (instance owned by the caller until destroy is called), then
 * activates it in place of the current face. On failure the previous face
 * stays, or the fallback if there was none. Returns the face's own result. */
int32_t roundie_face_host_switch(roundie_face_host_t *host, const rwf_face_v1_t *face, void *instance,
                                 const char *id, rwf_surface_t *surface, const rwf_data_v1_t *data);
/* The built-in fallback in place of the current face (also "return to default"). */
int32_t roundie_face_host_fallback(roundie_face_host_t *host, rwf_surface_t *surface, const rwf_data_v1_t *data);
/* Light sleep (M8-B): deactivate before the GUI parks; resume activates again
 * and delivers RWF_EVENT_RESUMED with the current sample (no missed ticks). */
void roundie_face_host_park(roundie_face_host_t *host);
int32_t roundie_face_host_resume(roundie_face_host_t *host, rwf_surface_t *surface, const rwf_data_v1_t *data);
/* Home left (an application or the list): deactivate, keep created. Showing
 * Home again is resume. */
void roundie_face_host_hide(roundie_face_host_t *host);
/* Delivers the events between the last sample and data. */
int32_t roundie_face_host_sample(roundie_face_host_t *host, const rwf_data_v1_t *data);
int32_t roundie_face_host_event(roundie_face_host_t *host, const rwf_event_v1_t *event, const rwf_data_v1_t *data);
/* Destroys the current face (shutdown, Safe Mode, tests). */
void roundie_face_host_clear(roundie_face_host_t *host);
uint32_t roundie_face_host_needs(const roundie_face_host_t *host);
bool roundie_face_host_active(const roundie_face_host_t *host);
/* Native backend admission (docs/watch-face-abi.md): BootGuard must be ARMED
 * and the vtable must negotiate; v1 then refuses with RWF_ERR_UNSUPPORTED
 * because it defines no native drawing interface. Never calls the face. */
int32_t roundie_face_host_admit_native(const roundie_face_host_t *host, const rwf_face_v1_t *face, bool guard_armed);
