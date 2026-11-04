#include "roundie_tone.h"

static const int16_t sine_table[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602,
    -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static int32_t sine_at(uint32_t phase)
{
    uint32_t index = phase >> 24;
    uint32_t fraction = (phase >> 8) & 0xffffU; /* 16 bits between two entries */
    int32_t a = sine_table[index];
    int32_t b = sine_table[(index + 1U) & 0xffU];
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)fraction) >> 16);
}

int roundie_tone_init(roundie_tone_t *tone, uint32_t frequency_hz, uint32_t sample_rate,
                      uint32_t duration_ms, int32_t amplitude)
{
    if (frequency_hz < ROUNDIE_TONE_MIN_HZ || frequency_hz > ROUNDIE_TONE_MAX_HZ || sample_rate < 8000U ||
        sample_rate > 48000U || frequency_hz * 2U > sample_rate) {
        return 0;
    }
    if (amplitude < 0) {
        amplitude = 0;
    }
    if (amplitude > 32767) {
        amplitude = 32767;
    }
    uint64_t step = ((uint64_t)frequency_hz << 32) / sample_rate;
    uint64_t frames = (uint64_t)sample_rate * duration_ms / 1000U;
    tone->phase = 0;
    tone->step = (uint32_t)step;
    tone->amplitude = amplitude;
    tone->total_frames = (uint32_t)frames;
    tone->frames_left = tone->total_frames;
    tone->frames_done = 0;
    tone->fade_frames = sample_rate * ROUNDIE_TONE_FADE_MS / 1000U;
    if (tone->fade_frames * 2U > tone->total_frames) {
        tone->fade_frames = tone->total_frames / 2U;
    }
    return 1;
}

uint32_t roundie_tone_fill(roundie_tone_t *tone, int16_t *stereo, uint32_t frames)
{
    uint32_t count = frames < tone->frames_left ? frames : tone->frames_left;
    for (uint32_t index = 0; index < count; index++) {
        int32_t gain = 32768; /* Q15, 1.0 */
        if (tone->frames_done < tone->fade_frames) {
            gain = (int32_t)((uint64_t)tone->frames_done * 32768U / tone->fade_frames);
        } else if (tone->frames_left <= tone->fade_frames) {
            gain = (int32_t)((uint64_t)tone->frames_left * 32768U / tone->fade_frames);
        }
        int32_t sample = (int32_t)(((int64_t)sine_at(tone->phase) * tone->amplitude) >> 15);
        sample = (int32_t)(((int64_t)sample * gain) >> 15);
        stereo[2U * index] = (int16_t)sample;
        stereo[2U * index + 1U] = (int16_t)sample;
        tone->phase += tone->step;
        tone->frames_done++;
        tone->frames_left--;
    }
    return count;
}
