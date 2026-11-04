#pragma once
#include <stdbool.h>

/* RA1: bounded audio diagnostics on the shared USB Serial/JTAG framer, one
 * request and one reply per line. Development only (CONFIG_ROUNDIE_DEV_AUDIO).
 *   !RA1 <8 hex id> STATUS
 *   !RA1 <8 hex id> TONE <hz> <ms> <volume percent>
 *   !RA1 <8 hex id> STOP */
void roundie_audio_command(char *line, bool discard);
