#pragma once
#include "roundie_elf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* One persistent system executor; no direct invocation by diagnostics/GUI. */
bool roundie_elf_prepare(void);
void roundie_elf_cancel_prepare(void);
void roundie_elf_bind_executor(TaskHandle_t task);
bool roundie_elf_execute(const uint8_t *bytes, size_t size);
