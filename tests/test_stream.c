#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../components/roundie_audio/stream.c"

#define S(seconds) (INT64_C(1000000) * (seconds))

static uint8_t storage[1024];
static roundie_ring_t ring;

static void *producer(void *argument)
{
    (void)argument;
    uint32_t value = 0;
    uint8_t chunk[37];
    unsigned long total = 0;
    while (total < 200000) {
        for (size_t index = 0; index < sizeof(chunk); index++) {
            chunk[index] = (uint8_t)(value + index);
        }
        size_t put = roundie_ring_write(&ring, chunk, sizeof(chunk));
        value += (uint32_t)put; /* only the accepted bytes advance the sequence */
        total += put;
    }
    return NULL;
}

int main(void)
{
    /* Ring: capacity rules, wrap-around, partial writes, clear. */
    assert(!roundie_ring_init(&ring, storage, 1000) && !roundie_ring_init(&ring, storage, 0));
    assert(!roundie_ring_init(&ring, NULL, 1024) && roundie_ring_init(&ring, storage, 1024));
    uint8_t in[2048], out[2048];
    for (unsigned index = 0; index < sizeof(in); index++) {
        in[index] = (uint8_t)(index * 7);
    }
    assert(roundie_ring_write(&ring, in, 1500) == 1024 && roundie_ring_free(&ring) == 0);
    assert(roundie_ring_write(&ring, in, 1) == 0);
    assert(roundie_ring_read(&ring, out, 600) == 600 && memcmp(out, in, 600) == 0);
    assert(roundie_ring_write(&ring, in + 1024, 500) == 500); /* wraps */
    assert(roundie_ring_used(&ring) == 924);
    assert(roundie_ring_read(&ring, out, 2000) == 924);
    assert(memcmp(out, in + 600, 424) == 0 && memcmp(out + 424, in + 1024, 500) == 0);
    assert(roundie_ring_read(&ring, out, 10) == 0);
    roundie_ring_write(&ring, in, 100);
    roundie_ring_clear(&ring);
    assert(roundie_ring_used(&ring) == 0 && roundie_ring_free(&ring) == 1024);

    /* Two threads: every byte arrives once, in order. */
    assert(roundie_ring_init(&ring, storage, 1024));
    pthread_t thread;
    pthread_create(&thread, NULL, producer, NULL);
    unsigned long received = 0;
    uint32_t expected = 0;
    uint8_t chunk[61];
    while (received < 200000) {
        size_t got = roundie_ring_read(&ring, chunk, sizeof(chunk));
        for (size_t index = 0; index < got; index++) {
            assert(chunk[index] == (uint8_t)expected);
            expected++;
        }
        received += got;
    }
    pthread_join(thread, NULL);
    assert(received >= 200000);

    /* Playback policy: prebuffer, play, underrun, pause, resume. */
    roundie_playback_t playback;
    roundie_playback_init(&playback, 4096, 500000);
    assert(roundie_playback_step(&playback, 100, 512, 0) == ROUNDIE_PLAYBACK_ACTION_WAIT);
    assert(roundie_playback_step(&playback, 4095, 512, 0) == ROUNDIE_PLAYBACK_ACTION_WAIT);
    assert(roundie_playback_step(&playback, 4096, 512, 0) == ROUNDIE_PLAYBACK_ACTION_START);
    assert(playback.phase == ROUNDIE_PLAYBACK_PLAYING);
    assert(roundie_playback_step(&playback, 3000, 512, 1000) == ROUNDIE_PLAYBACK_ACTION_PLAY);
    assert(roundie_playback_step(&playback, 100, 512, 2000) == ROUNDIE_PLAYBACK_ACTION_SILENCE);
    assert(playback.phase == ROUNDIE_PLAYBACK_UNDERRUN && playback.underruns == 1);
    assert(roundie_playback_step(&playback, 100, 512, 300000) == ROUNDIE_PLAYBACK_ACTION_SILENCE);
    assert(roundie_playback_step(&playback, 600, 512, 310000) == ROUNDIE_PLAYBACK_ACTION_PLAY); /* recovered */
    assert(playback.phase == ROUNDIE_PLAYBACK_PLAYING && playback.underruns == 1);
    roundie_playback_step(&playback, 0, 512, S(1));
    assert(roundie_playback_step(&playback, 0, 512, S(1) + 499999) == ROUNDIE_PLAYBACK_ACTION_SILENCE);
    assert(roundie_playback_step(&playback, 0, 512, S(1) + 500000) == ROUNDIE_PLAYBACK_ACTION_PAUSE);
    assert(playback.phase == ROUNDIE_PLAYBACK_PAUSED && playback.pauses == 1);
    assert(roundie_playback_step(&playback, 2000, 512, S(3)) == ROUNDIE_PLAYBACK_ACTION_WAIT); /* needs the prebuffer */
    assert(roundie_playback_step(&playback, 4096, 512, S(3)) == ROUNDIE_PLAYBACK_ACTION_START);

    /* Drain: what is left is played, then it finishes, from any phase, even below the prebuffer. */
    roundie_playback_init(&playback, 4096, 500000);
    playback.drain = 1;
    assert(roundie_playback_step(&playback, 0, 512, 0) == ROUNDIE_PLAYBACK_ACTION_FINISH);
    assert(roundie_playback_step(&playback, 300, 512, 0) == ROUNDIE_PLAYBACK_ACTION_START);
    assert(roundie_playback_step(&playback, 300, 512, 0) == ROUNDIE_PLAYBACK_ACTION_PLAY); /* a partial last chunk */
    assert(roundie_playback_step(&playback, 0, 512, 0) == ROUNDIE_PLAYBACK_ACTION_FINISH);
    roundie_playback_init(&playback, 4096, 500000);
    roundie_playback_step(&playback, 4096, 512, 0);
    roundie_playback_step(&playback, 0, 512, 1);
    assert(playback.phase == ROUNDIE_PLAYBACK_UNDERRUN);
    playback.drain = 1;
    assert(roundie_playback_step(&playback, 0, 512, 2) == ROUNDIE_PLAYBACK_ACTION_FINISH);

    /* PCM conversion: mono duplicates, stereo keeps channels, gain scales, clipping saturates. */
    int16_t mono[4] = {1000, -1000, 32767, -32768};
    int16_t stereo[8];
    roundie_pcm_to_stereo(mono, 4, 1, 32768, stereo);
    assert(stereo[0] == 1000 && stereo[1] == 1000 && stereo[2] == -1000 && stereo[3] == -1000);
    assert(stereo[4] == 32767 && stereo[7] == -32768);
    roundie_pcm_to_stereo(mono, 4, 1, 16384, stereo); /* half gain */
    assert(stereo[0] == 500 && stereo[2] == -500);
    int16_t two[4] = {100, -200, 300, -400};
    roundie_pcm_to_stereo(two, 2, 2, 32768, stereo);
    assert(stereo[0] == 100 && stereo[1] == -200 && stereo[2] == 300 && stereo[3] == -400);
    roundie_pcm_to_stereo(two, 2, 2, 0, stereo);
    assert(stereo[0] == 0 && stereo[3] == 0);
    roundie_pcm_to_stereo(mono, 4, 1, 65536, stereo); /* gain 2.0 saturates, never wraps */
    assert(stereo[4] == 32767 && stereo[6] == -32768);
    puts("stream: ring, two-thread ordering, playback policy, drain and PCM conversion passed");
    return 0;
}
