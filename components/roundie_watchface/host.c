#include "roundie_face_host.h"
#include <string.h>
#include "roundie_face_text.h"

/* The host's own service table: the face sees the host as context; data,
 * resources and settings go to the owner's entries. */
static rwf_host_v1_t owner;

static bool service_data(void *context, rwf_data_v1_t *out)
{
    (void)context;
    return owner.data != NULL && owner.data(owner.context, out);
}

static void service_refresh(void *context)
{
    roundie_face_host_t *host = context;
    host->refresh_requested = true;
}

static void service_needs(void *context, uint32_t needs)
{
    roundie_face_host_t *host = context;
    host->requested_needs = needs & (RWF_NEED_SECOND | RWF_NEED_BATTERY | RWF_NEED_TAP);
    host->needs_changed = true;
}

static bool service_resource(void *context, uint32_t index, rwf_resource_v1_t *out)
{
    (void)context;
    return owner.resource != NULL && owner.resource(owner.context, index, out);
}

static int32_t service_setting_get(void *context, uint32_t index)
{
    (void)context;
    return owner.setting_get != NULL ? owner.setting_get(owner.context, index) : RWF_ERR_UNSUPPORTED;
}

static int32_t service_setting_set(void *context, uint32_t index, uint32_t value)
{
    (void)context;
    return owner.setting_set != NULL ? owner.setting_set(owner.context, index, value) : RWF_ERR_UNSUPPORTED;
}

void roundie_face_host_init(roundie_face_host_t *host, const rwf_host_v1_t *services,
                            const rwf_face_v1_t *fallback_face, void *fallback_instance)
{
    memset(host, 0, sizeof(*host));
    owner = *services;
    host->services = (rwf_host_v1_t){
        .size = sizeof(rwf_host_v1_t),
        .abi_major = RWF_ABI_MAJOR,
        .abi_minor = RWF_ABI_MINOR,
        .caps = services->caps,
        .context = host,
        .data = service_data,
        .request_refresh = service_refresh,
        .set_needs = service_needs,
        .resource = service_resource,
        .setting_get = service_setting_get,
        .setting_set = service_setting_set,
    };
    host->fallback_face = fallback_face;
    host->fallback_instance = fallback_instance;
}

int32_t roundie_face_host_check(const roundie_face_host_t *host, const rwf_face_v1_t *face)
{
    if (face == NULL || face->size < sizeof(rwf_face_v1_t)) {
        return RWF_ERR_INVALID;
    }
    if (face->abi_major != RWF_ABI_MAJOR || face->abi_minor > RWF_ABI_MINOR ||
        (face->requires & ~host->services.caps) != 0) {
        return RWF_ERR_UNSUPPORTED;
    }
    if (face->create == NULL || face->activate == NULL || face->update == NULL || face->deactivate == NULL ||
        face->destroy == NULL) {
        return RWF_ERR_INVALID;
    }
    return RWF_OK;
}

static void slot_deactivate(roundie_face_host_t *host, roundie_face_slot_t *slot)
{
    if (slot->state == ROUNDIE_FACE_ACTIVE) {
        slot->face->deactivate(slot->instance);
        slot->state = ROUNDIE_FACE_CREATED;
        host->deactivations++;
    }
}

static void slot_destroy(roundie_face_host_t *host, roundie_face_slot_t *slot)
{
    slot_deactivate(host, slot);
    if (slot->state == ROUNDIE_FACE_CREATED) {
        slot->face->destroy(slot->instance);
        host->destroys++;
    }
    *slot = (roundie_face_slot_t){0};
}

/* Applies a set_needs made during a call; the definition's needs otherwise. */
static void settle_needs(roundie_face_host_t *host, roundie_face_slot_t *slot)
{
    if (host->needs_changed) {
        slot->needs = host->requested_needs;
        host->needs_changed = false;
    }
}

static int32_t slot_activate(roundie_face_host_t *host, roundie_face_slot_t *slot, rwf_surface_t *surface,
                             const rwf_data_v1_t *data, uint32_t first_event)
{
    host->needs_changed = false;
    int32_t result = slot->face->activate(slot->instance, surface);
    if (result != RWF_OK) {
        /* A partial activation is cleaned up by deactivate, which cannot fail. */
        slot->face->deactivate(slot->instance);
        host->needs_changed = false;
        return result;
    }
    slot->state = ROUNDIE_FACE_ACTIVE;
    host->activations++;
    settle_needs(host, slot);
    rwf_event_v1_t event = {.size = sizeof(event), .kind = first_event};
    host->needs_changed = false;
    result = slot->face->update(slot->instance, &event, data);
    host->updates++;
    settle_needs(host, slot);
    if (result != RWF_OK) {
        slot_deactivate(host, slot);
        return result;
    }
    host->last = *data;
    host->have_sample = true;
    return RWF_OK;
}

static void record_failure(roundie_face_host_t *host, const roundie_face_slot_t *slot, int32_t result)
{
    host->failures++;
    host->last_error = result;
    memcpy(host->failed_id, slot->id, sizeof(host->failed_id));
}

int32_t roundie_face_host_fallback(roundie_face_host_t *host, rwf_surface_t *surface, const rwf_data_v1_t *data)
{
    if (host->current.state != ROUNDIE_FACE_EMPTY && host->current.fallback) {
        if (host->current.state == ROUNDIE_FACE_ACTIVE || host->parked) {
            return RWF_OK;
        }
    } else {
        slot_destroy(host, &host->current);
    }
    host->surface = surface;
    roundie_face_slot_t *slot = &host->current;
    if (slot->state == ROUNDIE_FACE_EMPTY) {
        *slot = (roundie_face_slot_t){
            .face = host->fallback_face,
            .instance = host->fallback_instance,
            .needs = host->fallback_face->needs,
            .fallback = true,
        };
        memcpy(slot->id, "system", sizeof("system"));
        host->needs_changed = false;
        int32_t result = slot->face->create(slot->instance, &host->services);
        settle_needs(host, slot);
        if (result != RWF_OK) {
            *slot = (roundie_face_slot_t){0};
            host->last_error = result;
            return result;
        }
        slot->state = ROUNDIE_FACE_CREATED;
        host->creates++;
    }
    if (host->parked) {
        return RWF_OK;
    }
    int32_t result = slot_activate(host, slot, surface, data, RWF_EVENT_ACTIVATED);
    if (result != RWF_OK) {
        host->last_error = result; /* the fallback stays created; the GUI retries later */
    }
    return result;
}

int32_t roundie_face_host_switch(roundie_face_host_t *host, const rwf_face_v1_t *face, void *instance,
                                 const char *id, rwf_surface_t *surface, const rwf_data_v1_t *data)
{
    int32_t result = roundie_face_host_check(host, face);
    if (result != RWF_OK || host->parked) {
        host->refused++;
        return result != RWF_OK ? result : RWF_ERR_STATE;
    }
    roundie_face_slot_t candidate = {.face = face, .instance = instance, .needs = face->needs};
    size_t length = strnlen(id, sizeof(candidate.id) - 1);
    memcpy(candidate.id, id, length);
    candidate.id[length] = '\0';
    host->needs_changed = false;
    result = face->create(instance, &host->services);
    settle_needs(host, &candidate);
    if (result != RWF_OK) {
        record_failure(host, &candidate, result);
        if (host->current.state == ROUNDIE_FACE_EMPTY) {
            roundie_face_host_fallback(host, surface, data);
        }
        return result;
    }
    candidate.state = ROUNDIE_FACE_CREATED;
    host->creates++;
    /* Never two active faces: the old one is only deactivated until the new one works. */
    slot_deactivate(host, &host->current);
    host->surface = surface;
    result = slot_activate(host, &candidate, surface, data, RWF_EVENT_ACTIVATED);
    if (result != RWF_OK) {
        record_failure(host, &candidate, result);
        slot_destroy(host, &candidate);
        if (host->current.state == ROUNDIE_FACE_CREATED &&
            slot_activate(host, &host->current, surface, data, RWF_EVENT_ACTIVATED) == RWF_OK) {
            return result;
        }
        slot_destroy(host, &host->current);
        roundie_face_host_fallback(host, surface, data);
        return result;
    }
    slot_destroy(host, &host->current);
    host->current = candidate;
    host->switches++;
    return RWF_OK;
}

void roundie_face_host_park(roundie_face_host_t *host)
{
    slot_deactivate(host, &host->current);
    host->parked = true;
}

void roundie_face_host_hide(roundie_face_host_t *host)
{
    roundie_face_host_park(host);
}

int32_t roundie_face_host_resume(roundie_face_host_t *host, rwf_surface_t *surface, const rwf_data_v1_t *data)
{
    host->parked = false;
    host->surface = surface;
    if (host->current.state == ROUNDIE_FACE_EMPTY) {
        return roundie_face_host_fallback(host, surface, data);
    }
    if (host->current.state == ROUNDIE_FACE_ACTIVE) {
        return RWF_OK;
    }
    int32_t result = slot_activate(host, &host->current, surface, data, RWF_EVENT_RESUMED);
    if (result != RWF_OK && !host->current.fallback) {
        record_failure(host, &host->current, result);
        slot_destroy(host, &host->current);
        roundie_face_host_fallback(host, surface, data);
    }
    return result;
}

static int32_t deliver(roundie_face_host_t *host, const rwf_event_v1_t *event, const rwf_data_v1_t *data)
{
    roundie_face_slot_t *slot = &host->current;
    host->needs_changed = false;
    int32_t result = slot->face->update(slot->instance, event, data);
    host->updates++;
    settle_needs(host, slot);
    if (result != RWF_OK && !slot->fallback) {
        record_failure(host, slot, result);
        slot_destroy(host, slot);
        roundie_face_host_fallback(host, host->surface, data);
    }
    return result;
}

int32_t roundie_face_host_sample(roundie_face_host_t *host, const rwf_data_v1_t *data)
{
    if (host->parked || host->current.state != ROUNDIE_FACE_ACTIVE) {
        return RWF_ERR_STATE;
    }
    uint32_t kinds[8];
    unsigned count = roundie_face_changes(host->have_sample ? &host->last : NULL, data, host->current.needs,
                                          kinds, 8);
    host->last = *data;
    host->have_sample = true;
    for (unsigned i = 0; i < count; i++) {
        rwf_event_v1_t event = {.size = sizeof(event), .kind = kinds[i]};
        int32_t result = deliver(host, &event, data);
        if (result != RWF_OK) {
            return result;
        }
    }
    if (host->refresh_requested && host->current.state == ROUNDIE_FACE_ACTIVE) {
        host->refresh_requested = false;
        rwf_event_v1_t event = {.size = sizeof(event), .kind = RWF_EVENT_REFRESH};
        return deliver(host, &event, data);
    }
    return RWF_OK;
}

int32_t roundie_face_host_event(roundie_face_host_t *host, const rwf_event_v1_t *event, const rwf_data_v1_t *data)
{
    if (host->parked || host->current.state != ROUNDIE_FACE_ACTIVE) {
        return RWF_ERR_STATE;
    }
    return deliver(host, event, data);
}

void roundie_face_host_clear(roundie_face_host_t *host)
{
    slot_destroy(host, &host->current);
    host->have_sample = false;
}

uint32_t roundie_face_host_needs(const roundie_face_host_t *host)
{
    return host->current.state == ROUNDIE_FACE_ACTIVE ? host->current.needs : 0U;
}

bool roundie_face_host_active(const roundie_face_host_t *host)
{
    return host->current.state == ROUNDIE_FACE_ACTIVE;
}

int32_t roundie_face_host_admit_native(const roundie_face_host_t *host, const rwf_face_v1_t *face, bool guard_armed)
{
    if (!guard_armed) {
        return RWF_ERR_STATE; /* BootGuard not ARMED (FAILED, trial): the fallback only */
    }
    int32_t result = roundie_face_host_check(host, face);
    if (result != RWF_OK) {
        return result;
    }
    /* ABI v1 gives native code no drawing surface it could use without LVGL
     * in the public ABI: a valid native face is refused, not run. */
    return RWF_ERR_UNSUPPORTED;
}
