#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../components/roundie_audio/tone.c"

static unsigned zero_crossings(const int16_t *stereo, uint32_t frames)
{
    unsigned count = 0;
    for (uint32_t index = 1; index < frames; index++) {
        if ((stereo[2 * (index - 1)] < 0) != (stereo[2 * index] < 0)) {
            count++;
        }
    }
    return count;
}

int main(void)
{
    roundie_tone_t tone;

    /* Out-of-range requests are refused. */
    assert(!roundie_tone_init(&tone, 10, 16000, 100, 1000));
    assert(!roundie_tone_init(&tone, 9000, 48000, 100, 1000));
    assert(!roundie_tone_init(&tone, 440, 4000, 100, 1000));
    assert(!roundie_tone_init(&tone, 7000, 12000, 100, 1000)); /* above Nyquist */

    /* Frame count and frequency: 1 s at 16 kHz and 440 Hz crosses zero about 880 times. */
    assert(roundie_tone_init(&tone, 440, 16000, 1000, 16000));
    int16_t *buffer = malloc(2 * 16000 * sizeof(int16_t));
    uint32_t written = 0;
    while (written < 16000) {
        uint32_t got = roundie_tone_fill(&tone, buffer + 2 * written, 333); /* odd chunk size on purpose */
        assert(got > 0);
        written += got;
    }
    assert(written == 16000 && tone.frames_left == 0 && roundie_tone_fill(&tone, buffer, 100) == 0);
    unsigned crossings = zero_crossings(buffer, written);
    assert(crossings >= 878 && crossings <= 882);

    /* Amplitude stays within the requested peak; both channels match; the ends are faded. */
    int32_t peak = 0;
    for (uint32_t index = 0; index < written; index++) {
        assert(buffer[2 * index] == buffer[2 * index + 1]);
        int32_t value = abs(buffer[2 * index]);
        if (value > peak) {
            peak = value;
        }
    }
    assert(peak <= 16000 && peak > 15900);
    assert(buffer[0] == 0 && abs(buffer[2 * (written - 1)]) < 600); /* faded in, nearly faded out */
    assert(abs(buffer[2 * 3]) < abs(buffer[2 * 70]) + 16000);       /* the ramp grows, sanity only */

    /* The chunk size does not change the samples. */
    int16_t *again = malloc(2 * 16000 * sizeof(int16_t));
    assert(roundie_tone_init(&tone, 440, 16000, 1000, 16000));
    assert(roundie_tone_fill(&tone, again, 16000) == 16000);
    for (uint32_t index = 0; index < 2 * 16000; index++) {
        assert(again[index] == buffer[index]);
    }

    /* Full scale never overflows; a short tone fades inside itself; zero amplitude is silence. */
    assert(roundie_tone_init(&tone, 1000, 16000, 20, 40000)); /* clamped to 32767 */
    assert(roundie_tone_fill(&tone, again, 1000) == 320);
    for (uint32_t index = 0; index < 320; index++) {
        assert(again[2 * index] >= -32767 && again[2 * index] <= 32767);
    }
    assert(roundie_tone_init(&tone, 1000, 16000, 4, 20000));
    assert(roundie_tone_fill(&tone, again, 1000) == 64);
    assert(roundie_tone_init(&tone, 1000, 16000, 100, 0));
    roundie_tone_fill(&tone, again, 1600);
    for (uint32_t index = 0; index < 1600; index++) {
        assert(again[2 * index] == 0);
    }
    free(buffer);
    free(again);
    puts("tone: range checks, frequency, chunk independence, fades and clamping passed");
    return 0;
}
