#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Pure parts of the audio stream (docs/audio.md, gate A2): a single-producer,
 * single-consumer byte ring, the playback policy (prebuffer, underrun, pause)
 * and the PCM conversion done before the codec. Integer arithmetic, no IDF
 * calls, no allocation; host-tested. */

typedef struct {
    uint8_t *data;
    size_t capacity; /* a power of two */
    _Atomic size_t head; /* total bytes written */
    _Atomic size_t tail; /* total bytes read */
} roundie_ring_t;

/* capacity must be a power of two; returns 0 otherwise. */
int roundie_ring_init(roundie_ring_t *ring, uint8_t *storage, size_t capacity);
size_t roundie_ring_used(const roundie_ring_t *ring);
size_t roundie_ring_free(const roundie_ring_t *ring);
/* Producer only. Copies as much as fits and returns that count. */
size_t roundie_ring_write(roundie_ring_t *ring, const void *data, size_t length);
/* Consumer only. */
size_t roundie_ring_read(roundie_ring_t *ring, void *out, size_t length);
/* Consumer only, with the producer quiet: drops everything buffered. */
void roundie_ring_clear(roundie_ring_t *ring);

typedef enum {
    ROUNDIE_PLAYBACK_WAITING,  /* codec closed, filling up to the prebuffer */
    ROUNDIE_PLAYBACK_PLAYING,
    ROUNDIE_PLAYBACK_UNDERRUN, /* ring empty while playing: silence is fed */
    ROUNDIE_PLAYBACK_PAUSED,   /* underrun lasted: codec closed, amplifier off */
} roundie_playback_phase_t;

typedef enum {
    ROUNDIE_PLAYBACK_ACTION_WAIT,    /* nothing to do now */
    ROUNDIE_PLAYBACK_ACTION_START,   /* open the codec, then play a chunk */
    ROUNDIE_PLAYBACK_ACTION_PLAY,    /* play a chunk from the ring */
    ROUNDIE_PLAYBACK_ACTION_SILENCE, /* play a chunk of silence */
    ROUNDIE_PLAYBACK_ACTION_PAUSE,   /* close the codec */
    ROUNDIE_PLAYBACK_ACTION_FINISH,  /* the drain is complete: close and release */
} roundie_playback_action_t;

typedef struct {
    roundie_playback_phase_t phase;
    int64_t underrun_since_us;
    uint32_t underruns;   /* playing to underrun transitions */
    uint32_t pauses;
    size_t prebuffer_bytes;
    int64_t pause_after_us;
    int drain;            /* the owner closed with drain: play what is left, then finish */
} roundie_playback_t;

void roundie_playback_init(roundie_playback_t *playback, size_t prebuffer_bytes, int64_t pause_after_us);
/* One decision per chunk. used_bytes: what the ring holds; chunk_bytes: one codec write. */
roundie_playback_action_t roundie_playback_step(roundie_playback_t *playback, size_t used_bytes,
                                                size_t chunk_bytes, int64_t now_us);

/* Mono or stereo 16-bit PCM to stereo with a Q15 gain (32768 = 1.0), saturating. */
void roundie_pcm_to_stereo(const int16_t *input, uint32_t frames, uint8_t channels, uint32_t gain_q15,
                           int16_t *stereo);
