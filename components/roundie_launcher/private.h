#pragma once
#include "roundie_launcher.h"
void roundie_launcher_gui_report(bool ready, bool foreground, uint32_t objects,
    uint32_t pool, uint32_t create_us, uint32_t latency_us);
void roundie_launcher_gui_failed(void);
