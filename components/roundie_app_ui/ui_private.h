#pragma once
#include "roundie_app_ui.h"
typedef struct {
    uint32_t generation;
    uint32_t handle;
    uint32_t operation; /* 0 create, otherwise REP_UI property */
    uint32_t value; /* VISIBLE/ENABLED 0 or 1; VALUE int32 bits */
    int64_t posted_us;
    rep_ui_widget_v2_t widget;
} roundie_ui_command_t;
/* GUI-only backend. No locks held over these calls. */
bool roundie_ui_render(const roundie_ui_command_t *command);
bool roundie_ui_render_show(bool visible);
void roundie_ui_render_destroy(void);
uint32_t roundie_ui_render_bytes(void);
void roundie_ui_render_metrics(uint32_t *before, uint32_t *free, uint32_t *objects);
/* Sets a round widget's LVGL value as touch would, then reports it as INJECTED. */
bool roundie_ui_render_inject(uint32_t generation, uint32_t handle, int32_t value);
/* Item count of "a\nb" text: LIST items 1..16, DIALOG fields 2..3; 0 if invalid. */
uint32_t roundie_ui_text_fields(const char *text, uint32_t length);
/* Whole box inside the round panel with a 4-pixel margin (REP section 12.3). */
bool roundie_ui_box_in_circle(int32_t x, int32_t y, int32_t width, int32_t height);
_Static_assert(sizeof(roundie_ui_command_t) == 176, "UI copied command storage");
