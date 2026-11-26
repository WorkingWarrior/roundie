#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rep/rep_app_v3.h"

/* The UI canvas in pixels: the coordinate space of the launcher, system screens
 * and REP applications (ABI 2 and 3 widgets are validated against it). Part of
 * the application contract, so it does not follow the board; the GUI checks
 * at build time that the panel matches (docs/board-support.md). */
#define ROUNDIE_UI_WIDTH 360
#define ROUNDIE_UI_HEIGHT 360
#define ROUNDIE_UI_CENTER 180 /* x and y of the round glass's centre */
#define ROUNDIE_UI_SAFE_RADIUS 176 /* round widgets keep every corner within it */

#define ROUNDIE_UI_QUEUE_CAPACITY 12U
#define ROUNDIE_UI_BATCH 4U
/* BUTTON: payload = origin. VALUE: payload = committed int32 bits; coalesce
 * permits replacing an undelivered VALUE event of the same widget. */
typedef bool (*roundie_ui_event_sink_t)(uint32_t generation, uint32_t type, uint32_t widget,
                                      uint32_t payload, bool coalesce, int64_t posted_us);
typedef enum {
    ROUNDIE_UI_SHOW = 1, ROUNDIE_UI_HIDE, ROUNDIE_UI_DESTROY,
} roundie_ui_transition_t;
/* Cause of a refused create, update or destroy (roundie_ui_status_t). */
typedef enum {
    ROUNDIE_UI_REJECT_NONE = 0,
    ROUNDIE_UI_REJECT_FULL,       /* the command queue had no free entry */
    ROUNDIE_UI_REJECT_CLOSED,     /* other generation, admission closed (STOP) or view destroyed */
    ROUNDIE_UI_REJECT_TRANSITION, /* a SHOW/HIDE/DESTROY transition is still pending */
    ROUNDIE_UI_REJECT_ERROR,      /* the instance's UI is in error */
    ROUNDIE_UI_REJECT_REQUEST,    /* invalid request: format, handle, type, range or widget limit */
} roundie_ui_reject_t;
typedef struct {
    uint32_t generation;
    uint32_t view;
    uint32_t button;
    uint32_t widgets;
    uint32_t pending;
    uint32_t max_depth;
    uint32_t submitted;
    uint32_t applied;
    uint32_t failed;
    uint32_t rejected;
    uint32_t canceled;
    uint32_t input_accepted;
    uint32_t input_dropped;
    uint32_t input_rejected;
    uint32_t physical;
    uint32_t injected;
    uint32_t values;
    uint32_t delivered;
    uint32_t latency_max_us;
    uint32_t enqueue_max_us;
    uint32_t create_max_us;
    uint32_t update_max_us;
    uint32_t teardown_us;
    uint32_t input_latency_max_us;
    uint32_t acquired;
    uint32_t released;
    uint32_t outstanding;
    uint32_t lvgl_bytes;
    uint32_t lvgl_peak;
    uint32_t flushes;
    uint32_t flush_errors;
    uint32_t last_widget;
    uint32_t text_hash;
    uint32_t pool_before;
    uint32_t pool_free;
    uint32_t realized_objects;
    /* Refused create/update/destroy calls by cause (they sum to rejected),
     * and the last refusal: its cause, the queue depth and the time since the
     * GUI task last pumped the queue. Diagnostics, not part of the app ABI. */
    uint32_t rejected_full;
    uint32_t rejected_admission;
    uint32_t rejected_request;
    uint32_t reject_reason;
    uint32_t reject_depth;
    uint32_t reject_pump_age_us;
    int32_t error;
    bool foreground;
    bool admitting;
} roundie_ui_status_t;

/* Executor methods; pointers are validated/copied by ABI before entry. */
bool roundie_ui_open(uint32_t generation, roundie_ui_event_sink_t sink);
/* Version-1 requests arrive widened, with zero round fields. The caller gates
 * version 2, REP_UI_VALUE and shadow reads on REP3_CAP_ROUND_UI. */
uint32_t roundie_ui_create(uint32_t generation, const rep_ui_widget_v2_t *widget);
int32_t roundie_ui_update(uint32_t generation, const rep_ui_update_t *update);
int32_t roundie_ui_destroy(uint32_t generation, uint32_t view);
void roundie_ui_close_admission(uint32_t generation);
/* Only outside native callbacks. One reserved lifecycle mailbox; timeout leaves
 * ownership intact. DESTROY can be retried; success is the GUI deletion barrier. */
bool roundie_ui_transition(uint32_t generation, roundie_ui_transition_t operation);
bool roundie_ui_retire(uint32_t generation);
void roundie_ui_status(roundie_ui_status_t *out);
/* Shadow state copy under the bridge lock; never waits for the GUI. */
bool roundie_ui_shadow(uint32_t generation, uint32_t widget, int32_t *committed,
                       int32_t *requested, uint32_t *pending);
bool roundie_ui_input(uint32_t generation, uint32_t widget, uint32_t origin);
/* GUI task only: a round widget's committed value changed by input. */
bool roundie_ui_value_input(uint32_t generation, uint32_t widget, int32_t value, uint32_t origin);
/* Diagnostics: one pending injected value, applied by the GUI as if touched. */
bool roundie_ui_inject_value(uint32_t generation, uint32_t widget, int32_t value);
bool roundie_ui_event_valid(uint32_t generation, uint32_t type, uint32_t widget, uint32_t payload);
void roundie_ui_delivered(uint32_t generation, uint32_t latency_us);

/* Bind once from existing GUI task, then pump once per GUI iteration. No ELF
 * pointer or LVGL pointer crosses the executor/GUI mailbox. */
void roundie_ui_gui_bind(void);
void roundie_ui_gui_pump(void);
/* GUI task only. Development comparison uses the historical four-command
 * batch or the bounded full mailbox; no command is coalesced/discarded. */
void roundie_ui_gui_batch(unsigned limit);
void roundie_ui_display_progress(uint32_t completed, uint32_t errors);
