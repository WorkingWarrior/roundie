#ifndef REP_APP_V2_H
#define REP_APP_V2_H
#include <stddef.h>
#include <stdint.h>

/* Separate contract: ABI 1 headers and binaries are unchanged. Trusted native
 * callbacks run only on the executor and must return within 100 ms. No waits,
 * tasks, TLS, libc or driver APIs. All borrowed pointers expire on return. */
#define REP_ABI_V2 UINT32_C(2)
#define REP2_EVENT_TIMER UINT32_C(1)
#define REP2_EVENT_USER UINT32_C(2)
#define REP2_CAP_TIMER UINT32_C(8)
#define REP2_APP_METADATA \
    __attribute__((used, section(".rep.abi"))) \
    const uint32_t rep_abi_metadata[3] = {UINT32_C(0x31504552), REP_ABI_V2, 12}
#define REP2_EXPORT __attribute__((visibility("default")))

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t instance;
    uint32_t reserved;
} rep_context_v2_t;

typedef struct {
    uint32_t type;
    uint32_t struct_size;
    uint32_t value;
    uint32_t reserved;
    uint64_t posted_us;
} rep_event_v2_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t capabilities;
    int32_t (*log)(uint32_t instance, const char *text, uint32_t length);
    uint64_t (*monotonic_ms)(uint32_t instance);
    void *(*alloc)(uint32_t instance, uint32_t bytes);
    int32_t (*release)(uint32_t instance, void *pointer);
    /* One periodic source per application, 50..10000 ms. Zero means refused.
     * Handle identity never repeats in a boot; cancellation checks generation.
     * Timers pause in suspend and are canceled before stop/destroy. */
    uint32_t (*timer_start)(uint32_t instance, uint32_t period_ms);
    int32_t (*timer_cancel)(uint32_t instance, uint32_t handle);
} rep_api_v2_t;

typedef int32_t (*rep_callback_v2_t)(const rep_context_v2_t *context);
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    rep_callback_v2_t init;
    rep_callback_v2_t start;
    int32_t (*on_event)(const rep_context_v2_t *context, const rep_event_v2_t *event);
    rep_callback_v2_t suspend;
    rep_callback_v2_t resume;
    rep_callback_v2_t stop;
    rep_callback_v2_t destroy;
} rep_descriptor_v2_t;

REP2_EXPORT const rep_api_v2_t *rep_abi_v2(uint32_t required_version);
REP2_EXPORT const rep_descriptor_v2_t *rep_app_entry_v2(const rep_api_v2_t *api);

_Static_assert(sizeof(rep_context_v2_t) == 16, "REP2 context");
_Static_assert(sizeof(rep_event_v2_t) == 24, "REP2 event");
_Static_assert(offsetof(rep_event_v2_t, posted_us) == 16, "REP2 timestamp");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(rep_api_v2_t) == 36, "REP2 API");
_Static_assert(offsetof(rep_api_v2_t, timer_start) == 28, "REP2 timer");
_Static_assert(sizeof(rep_descriptor_v2_t) == 36, "REP2 descriptor");
_Static_assert(offsetof(rep_descriptor_v2_t, destroy) == 32, "REP2 destroy");
#elif defined(__x86_64__) && UINTPTR_MAX == UINT64_MAX /* linux-x64.rep1, System V LP64 */
_Static_assert(sizeof(rep_api_v2_t) == 64, "REP2 API");
_Static_assert(offsetof(rep_api_v2_t, timer_start) == 48, "REP2 timer");
_Static_assert(sizeof(rep_descriptor_v2_t) == 64, "REP2 descriptor");
_Static_assert(offsetof(rep_descriptor_v2_t, destroy) == 56, "REP2 destroy");
#endif
#endif
