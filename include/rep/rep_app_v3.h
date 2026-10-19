#ifndef REP_APP_V3_H
#define REP_APP_V3_H
#include "rep_app_v2.h"

/* ABI 3 is explicit. ABI 2 layouts are reused as value types, not extended.
 * All requests live in ELF data or SDK allocations (not executor stack).
 * Text is UTF-8, length excludes the mandatory trailing NUL. */
#define REP_ABI_V3 UINT32_C(3)
#define REP3_CAP_UI UINT32_C(16)
#define REP3_EVENT_BUTTON UINT32_C(3)
#define REP3_ORIGIN_TOUCH UINT32_C(1)
#define REP3_ORIGIN_INJECTED UINT32_C(2)
#define REP_UI_VIEW UINT32_C(1)
#define REP_UI_LABEL UINT32_C(2)
#define REP_UI_BUTTON UINT32_C(3)
#define REP_UI_TEXT UINT32_C(1)
#define REP_UI_VISIBLE UINT32_C(2)
#define REP_UI_ENABLED UINT32_C(3)
#define REP_UI_TEXT_CAPACITY 96U
#define REP_UI_WIDGET_LIMIT 8U /* includes the view */
#define REP3_APP_METADATA \
    __attribute__((used, section(".rep.abi"))) \
    const uint32_t rep_abi_metadata[3] = {UINT32_C(0x31504552), REP_ABI_V3, 12}

typedef rep_context_v2_t rep_context_v3_t;
/* For BUTTON: value = widget handle, reserved = event origin. The enclosing
 * callback context identifies generation. No raw touch or context pointers. */
typedef rep_event_v2_t rep_event_v3_t;

typedef struct {
    uint32_t version; /* 1 */
    uint32_t struct_size;
    uint32_t type;
    uint32_t parent; /* zero for view; otherwise exact current view handle */
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t text_length;
    char text[REP_UI_TEXT_CAPACITY];
} rep_ui_widget_t;

typedef struct {
    uint32_t version; /* 1 */
    uint32_t struct_size;
    uint32_t widget;
    uint32_t property;
    uint32_t value; /* TEXT: byte length; VISIBLE/ENABLED: 0 or 1 */
    char text[REP_UI_TEXT_CAPACITY];
} rep_ui_update_t;

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    uint32_t submitted;
    uint32_t applied;
    uint32_t failed;
    uint32_t pending;
    int32_t error; /* sticky GUI error; runtime tears down on failure */
    uint32_t view;
} rep_ui_result_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t capabilities;
    int32_t (*log)(uint32_t instance, const char *text, uint32_t length);
    uint64_t (*monotonic_ms)(uint32_t instance);
    void *(*alloc)(uint32_t instance, uint32_t bytes);
    int32_t (*release)(uint32_t instance, void *pointer);
    uint32_t (*timer_start)(uint32_t instance, uint32_t period_ms);
    int32_t (*timer_cancel)(uint32_t instance, uint32_t handle);
    /* Nonzero handle = accepted, not yet rendered. Handles never repeat in a
     * boot. Failures return 0 / -1. Query copied completion counters. */
    uint32_t (*ui_create)(uint32_t instance, const rep_ui_widget_t *widget);
    int32_t (*ui_update)(uint32_t instance, const rep_ui_update_t *update);
    int32_t (*ui_destroy)(uint32_t instance, uint32_t view);
    int32_t (*ui_result)(uint32_t instance, rep_ui_result_t *result);
} rep_api_v3_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    rep_callback_v2_t init;
    rep_callback_v2_t start;
    int32_t (*on_event)(const rep_context_v3_t *, const rep_event_v3_t *);
    rep_callback_v2_t suspend;
    rep_callback_v2_t resume;
    rep_callback_v2_t stop;
    rep_callback_v2_t destroy;
    uint32_t required_capabilities;
} rep_descriptor_v3_t;

REP2_EXPORT const rep_api_v3_t *rep_abi_v3(uint32_t required_version);
REP2_EXPORT const rep_descriptor_v3_t *rep_app_entry_v3(const rep_api_v3_t *api);
_Static_assert(sizeof(rep_ui_widget_t) == 132, "REP3 widget");
_Static_assert(offsetof(rep_ui_widget_t, text) == 36, "REP3 widget text");
_Static_assert(sizeof(rep_ui_update_t) == 116, "REP3 update");
_Static_assert(sizeof(rep_ui_result_t) == 32, "REP3 result");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(rep_api_v3_t) == 52, "REP3 API");
_Static_assert(offsetof(rep_api_v3_t, ui_create) == 36, "REP3 UI API");
_Static_assert(sizeof(rep_descriptor_v3_t) == 40, "REP3 descriptor");
_Static_assert(offsetof(rep_descriptor_v3_t, required_capabilities) == 36, "REP3 capabilities");
#endif
#endif
