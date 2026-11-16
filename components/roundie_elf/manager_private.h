#pragma once
#include "roundie_elf.h"
#include "roundie_platform.h"

/* One persistent system executor; no direct invocation by diagnostics/GUI. */
bool roundie_elf_prepare(void);
void roundie_elf_cancel_prepare(void);
void roundie_elf_bind_executor(roundie_thread_t task);
bool roundie_elf_execute(const uint8_t *bytes, size_t size);
void roundie_elf_step(int64_t now_us);
bool roundie_elf_input_busy(void);
