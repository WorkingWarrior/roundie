#pragma once
#include <stdbool.h>
#include <stdint.h>
void roundie_elf_command(char *line, bool discard);
void roundie_elf_poll(int64_t now_us);
bool roundie_elf_transfer_busy(void);
