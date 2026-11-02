/* Deliberately faulting application for physical recovery tests. Never ship it. */
#include "rep/rep_app_v2.h"

REP2_APP_METADATA;
static const rep_api_v2_t *services;
#define UNUSED __attribute__((unused))
static UNUSED volatile uint32_t *const volatile null_data = 0;
static UNUSED void (*const volatile null_code)(void) = 0;

static UNUSED uint32_t recurse(uint32_t depth)
{
    volatile uint8_t page[512];
    page[0] = (uint8_t)depth;
    return recurse(depth + 1) + page[0];
}

static void fail(void)
{
#if FAULT_KIND == 1
    for (;;) {
    }
#elif FAULT_KIND == 2
    (void)*null_data;
#elif FAULT_KIND == 3
    *null_data = 1;
#elif FAULT_KIND == 4
    __asm__ volatile("ill");
#elif FAULT_KIND == 5
    null_code();
#elif FAULT_KIND == 6
    __asm__ volatile("rsil a2, 15" : : : "a2");
    for (;;) {
    }
#elif FAULT_KIND == 7
    services->log(0, "", (uint32_t)recurse(0));
#else
#error "Unknown FAULT_KIND"
#endif
}

static int32_t pass(const rep_context_v2_t *context)
{
    (void)context;
    return 0;
}

static int32_t start(const rep_context_v2_t *context)
{
    return services->timer_start(context->instance, 250) == 0 ? -1 : 0;
}

static int32_t event(const rep_context_v2_t *context, const rep_event_v2_t *event)
{
    (void)context;
    (void)event;
    fail();
    return -2;
}

REP2_EXPORT const rep_descriptor_v2_t *rep_app_entry_v2(const rep_api_v2_t *api)
{
    static const rep_descriptor_v2_t descriptor = {
        REP_ABI_V2, sizeof(descriptor), pass, start, event, pass, pass, pass, pass,
    };
    if (api == NULL || api->abi_version != REP_ABI_V2 || api->struct_size != sizeof(*api) ||
        (api->capabilities & REP2_CAP_TIMER) == 0 || rep_abi_v2(REP_ABI_V2) != api) {
        return NULL;
    }
    services = api;
    return &descriptor;
}
