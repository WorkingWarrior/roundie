#include "rep/rep_app.h"

#ifndef REP_TEST_VARIANT
#define REP_TEST_VARIANT 0
#endif

#if REP_TEST_VARIANT == 2
__attribute__((used, section(".rep.abi")))
const uint32_t rep_abi_metadata[3] = {UINT32_C(0x31504552), 99, 12};
#else
REP_APP_METADATA;
#endif

static const rep_api_t *services;
static volatile uint32_t input = 40;
static uint32_t calculation;

static int32_t start(const rep_app_context_t *context)
{
    if (context->abi_version != REP_ABI_VERSION || context->event != REP_EVENT_START) {
        return -1;
    }
#if REP_TEST_VARIANT == 1
    extern void esp_restart(void);
    esp_restart(); /* Rejected at preflight, never executed. */
#endif
    static const char hello[] = "Hello from a native Roundie ELF application!";
    if (services->log(context->instance, hello, sizeof(hello) - 1) != 0) {
        return -2;
    }
    if (services->monotonic_ms(context->instance) == 0) {
        return -3;
    }
    calculation = input + 2;
    void *buffer = services->alloc(context->instance, 64);
    if (buffer == NULL) {
        return -4;
    }
    ((uint8_t *)buffer)[0] = (uint8_t)calculation;
    if (((uint8_t *)buffer)[0] != 42 || services->release(context->instance, buffer) != 0) {
        return -5;
    }
    static const char result[] = "REP ABI 1: 40 + 2 = 42; returning 42";
    services->log(context->instance, result, sizeof(result) - 1);
    return (int32_t)calculation;
}

REP_APP_EXPORT const rep_app_descriptor_t *rep_app_entry(const rep_api_t *api)
{
    static const rep_app_descriptor_t descriptor = {REP_ABI_VERSION, sizeof(descriptor), start};
    if (api == NULL || api->abi_version != REP_ABI_VERSION ||
        api->struct_size < sizeof(rep_api_t) || rep_abi_v1(REP_ABI_VERSION) != api ||
        rep_abi_v1(99) != NULL) {
        return NULL;
    }
    services = api;
    return &descriptor;
}
