#include "rep/rep_app_v2.h"

REP2_APP_METADATA;
static const rep_api_v2_t *services;
static struct {
    uint32_t ticks;
    uint32_t users;
    uint32_t last_value;
    uint32_t timer;
} *state;

#define MESSAGE(context, literal) services->log((context)->instance, literal, sizeof(literal) - 1)

static int32_t init(const rep_context_v2_t *context)
{
    state = services->alloc(context->instance, sizeof(*state));
    if (state == NULL) {
        return -1;
    }
    state->ticks = 0;
    state->users = 0;
    state->last_value = 0;
    state->timer = 0;
    return MESSAGE(context, "event app initialized");
}

static int32_t start(const rep_context_v2_t *context)
{
    state->timer = services->timer_start(context->instance, 250);
    if (state->timer == 0) {
        return -2;
    }
    return MESSAGE(context, "event app started");
}

static int32_t event(const rep_context_v2_t *context, const rep_event_v2_t *event)
{
    if (event->struct_size != sizeof(*event)) {
        return -3;
    }
    if (event->type == REP2_EVENT_TIMER) {
        state->ticks++;
        return MESSAGE(context, "tick");
    }
    if (event->type == REP2_EVENT_USER) {
        state->users++;
        state->last_value = event->value;
        return MESSAGE(context, "user event");
    }
    return -4;
}

static int32_t suspend(const rep_context_v2_t *context)
{
    return MESSAGE(context, "application suspended");
}

static int32_t resume(const rep_context_v2_t *context)
{
    return MESSAGE(context, "application resumed");
}

static int32_t stop(const rep_context_v2_t *context)
{
    return MESSAGE(context, "application stopping");
}

static int32_t destroy(const rep_context_v2_t *context)
{
    int32_t result = MESSAGE(context, "application destroyed");
    if (state != NULL) {
        if (services->release(context->instance, state) != 0) {
            result = -5;
        }
        state = NULL;
    }
    return result;
}

REP2_EXPORT const rep_descriptor_v2_t *rep_app_entry_v2(const rep_api_v2_t *api)
{
    static const rep_descriptor_v2_t descriptor = {
        REP_ABI_V2, sizeof(descriptor), init, start, event, suspend, resume, stop, destroy,
    };
    if (api == NULL || api->abi_version != REP_ABI_V2 || api->struct_size != sizeof(*api) ||
        (api->capabilities & REP2_CAP_TIMER) == 0 || rep_abi_v2(REP_ABI_V2) != api ||
        rep_abi_v2(1) != NULL) {
        return NULL;
    }
    services = api;
    return &descriptor;
}
