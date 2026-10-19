#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rep/rep_app_v3.h"

#define ROUNDIE_UI_QUEUE_CAPACITY 12U
#define ROUNDIE_UI_BATCH 4U
typedef bool (*roundie_ui_event_sink_t)(uint32_t generation, uint32_t widget,
                                      uint32_t origin, int64_t posted_us);
typedef enum {
    ROUNDIE_UI_SHOW = 1, ROUNDIE_UI_HIDE, ROUNDIE_UI_DESTROY,
} roundie_ui_transition_t;
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
    int32_t error;
    bool foreground;
    bool admitting;
} roundie_ui_status_t;

/* Executor methods; pointers are validated/copied by ABI before entry. */
bool roundie_ui_open(uint32_t generation, roundie_ui_event_sink_t sink);
uint32_t roundie_ui_create(uint32_t generation, const rep_ui_widget_t *widget);
int32_t roundie_ui_update(uint32_t generation, const rep_ui_update_t *update);
int32_t roundie_ui_destroy(uint32_t generation, uint32_t view);
void roundie_ui_close_admission(uint32_t generation);
/* Only outside native callbacks. One reserved lifecycle mailbox; timeout leaves
 * ownership intact. DESTROY can be retried; success is the GUI deletion barrier. */
bool roundie_ui_transition(uint32_t generation, roundie_ui_transition_t operation);
bool roundie_ui_retire(uint32_t generation);
void roundie_ui_status(roundie_ui_status_t *out);
bool roundie_ui_input(uint32_t generation, uint32_t widget, uint32_t origin);
bool roundie_ui_event_valid(uint32_t generation, uint32_t widget);
void roundie_ui_delivered(uint32_t generation, uint32_t latency_us);

/* Bind once from existing GUI task, then pump once per GUI iteration. No ELF
 * pointer or LVGL pointer crosses the executor/GUI mailbox. */
void roundie_ui_gui_bind(void);
void roundie_ui_gui_pump(void);
/* GUI task only. Development comparison uses the historical four-command
 * batch or the bounded full mailbox; no command is coalesced/discarded. */
void roundie_ui_gui_batch(unsigned limit);
void roundie_ui_display_progress(uint32_t completed, uint32_t errors);
