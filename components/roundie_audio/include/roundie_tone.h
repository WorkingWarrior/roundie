#pragma once
#include <stdint.h>

/* Sine tone generator for the audio system task: integer arithmetic only,
 * a phase accumulator over a 256-entry table with interpolation, and a short
 * linear fade at both ends so a tone starts and stops without a click. No IDF
 * calls, no allocation; the same code runs under host tests. */

#define ROUNDIE_TONE_MIN_HZ 50U
#define ROUNDIE_TONE_MAX_HZ 8000U
#define ROUNDIE_TONE_FADE_MS 5U

typedef struct {
    uint32_t phase;       /* 32-bit accumulator */
    uint32_t step;        /* phase increment per frame */
    int32_t amplitude;    /* peak value, 0..32767 */
    uint32_t frames_left;
    uint32_t frames_done;
    uint32_t fade_frames;
    uint32_t total_frames;
} roundie_tone_t;

/* Returns false for a frequency or sample rate out of range. */
int roundie_tone_init(roundie_tone_t *tone, uint32_t frequency_hz, uint32_t sample_rate,
                      uint32_t duration_ms, int32_t amplitude);
/* Writes up to frames stereo frames (left = right), returns how many it wrote;
 * 0 once the tone is complete. */
uint32_t roundie_tone_fill(roundie_tone_t *tone, int16_t *stereo, uint32_t frames);
