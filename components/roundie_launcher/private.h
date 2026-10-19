#pragma once
#include "roundie_launcher.h"
void roundie_launcher_gui_report(bool ready, bool foreground, uint32_t objects,
    uint32_t pool, uint32_t create_us, uint32_t latency_us);
void roundie_launcher_gui_failed(void);
void roundie_launcher_result_report(uint32_t request, bool visible, uint32_t apply_us,
    uint32_t completion_us);
void roundie_launcher_painted(roundie_navigation_t state, uint32_t request,
    uint32_t generation, int64_t at_us, uint32_t ready_us);
