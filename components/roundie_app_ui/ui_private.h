#pragma once
#include "roundie_app_ui.h"
typedef struct {
    uint32_t generation;
    uint32_t handle;
    uint32_t operation; /* 0 create, otherwise REP_UI property */
    uint32_t value;
    int64_t posted_us;
    rep_ui_widget_t widget;
} roundie_ui_command_t;
/* GUI-only backend. No locks held over these calls. */
bool roundie_ui_render(const roundie_ui_command_t *command);
bool roundie_ui_render_show(bool visible);
void roundie_ui_render_destroy(void);
uint32_t roundie_ui_render_bytes(void);
void roundie_ui_render_metrics(uint32_t *before, uint32_t *free, uint32_t *objects);
_Static_assert(sizeof(roundie_ui_command_t) == 160, "UI copied command storage");
