#include "roundie_stream.h"
#include <string.h>

int roundie_ring_init(roundie_ring_t *ring, uint8_t *storage, size_t capacity)
{
    if (storage == NULL || capacity == 0 || (capacity & (capacity - 1U)) != 0) {
        return 0;
    }
    ring->data = storage;
    ring->capacity = capacity;
    atomic_store(&ring->head, 0);
    atomic_store(&ring->tail, 0);
    return 1;
}

size_t roundie_ring_used(const roundie_ring_t *ring)
{
    return atomic_load_explicit(&ring->head, memory_order_acquire) -
        atomic_load_explicit(&ring->tail, memory_order_acquire);
}

size_t roundie_ring_free(const roundie_ring_t *ring)
{
    return ring->capacity - roundie_ring_used(ring);
}

size_t roundie_ring_write(roundie_ring_t *ring, const void *data, size_t length)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    size_t room = ring->capacity - (head - tail);
    size_t count = length < room ? length : room;
    size_t offset = head & (ring->capacity - 1U);
    size_t first = ring->capacity - offset;
    if (first > count) {
        first = count;
    }
    memcpy(ring->data + offset, data, first);
    memcpy(ring->data, (const uint8_t *)data + first, count - first);
    atomic_store_explicit(&ring->head, head + count, memory_order_release);
    return count;
}

size_t roundie_ring_read(roundie_ring_t *ring, void *out, size_t length)
{
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t available = head - tail;
    size_t count = length < available ? length : available;
    size_t offset = tail & (ring->capacity - 1U);
    size_t first = ring->capacity - offset;
    if (first > count) {
        first = count;
    }
    memcpy(out, ring->data + offset, first);
    memcpy((uint8_t *)out + first, ring->data, count - first);
    atomic_store_explicit(&ring->tail, tail + count, memory_order_release);
    return count;
}

void roundie_ring_clear(roundie_ring_t *ring)
{
    atomic_store_explicit(&ring->tail, atomic_load_explicit(&ring->head, memory_order_acquire),
                          memory_order_release);
}

void roundie_playback_init(roundie_playback_t *playback, size_t prebuffer_bytes, int64_t pause_after_us)
{
    *playback = (roundie_playback_t){
        .phase = ROUNDIE_PLAYBACK_WAITING,
        .prebuffer_bytes = prebuffer_bytes,
        .pause_after_us = pause_after_us,
    };
}

roundie_playback_action_t roundie_playback_step(roundie_playback_t *playback, size_t used_bytes,
                                                size_t chunk_bytes, int64_t now_us)
{
    switch (playback->phase) {
    case ROUNDIE_PLAYBACK_WAITING:
    case ROUNDIE_PLAYBACK_PAUSED:
        if (playback->drain && used_bytes == 0) {
            return ROUNDIE_PLAYBACK_ACTION_FINISH;
        }
        if (used_bytes >= playback->prebuffer_bytes || (playback->drain && used_bytes > 0)) {
            playback->phase = ROUNDIE_PLAYBACK_PLAYING;
            return ROUNDIE_PLAYBACK_ACTION_START;
        }
        return ROUNDIE_PLAYBACK_ACTION_WAIT;
    case ROUNDIE_PLAYBACK_PLAYING:
        if (used_bytes >= chunk_bytes || (playback->drain && used_bytes > 0)) {
            return ROUNDIE_PLAYBACK_ACTION_PLAY;
        }
        if (playback->drain) {
            return ROUNDIE_PLAYBACK_ACTION_FINISH;
        }
        playback->phase = ROUNDIE_PLAYBACK_UNDERRUN;
        playback->underrun_since_us = now_us;
        playback->underruns++;
        return ROUNDIE_PLAYBACK_ACTION_SILENCE;
    case ROUNDIE_PLAYBACK_UNDERRUN:
        if (used_bytes >= chunk_bytes || (playback->drain && used_bytes > 0)) {
            playback->phase = ROUNDIE_PLAYBACK_PLAYING;
            return ROUNDIE_PLAYBACK_ACTION_PLAY;
        }
        if (playback->drain) {
            return ROUNDIE_PLAYBACK_ACTION_FINISH;
        }
        if (now_us - playback->underrun_since_us >= playback->pause_after_us) {
            playback->phase = ROUNDIE_PLAYBACK_PAUSED;
            playback->pauses++;
            return ROUNDIE_PLAYBACK_ACTION_PAUSE;
        }
        return ROUNDIE_PLAYBACK_ACTION_SILENCE;
    }
    return ROUNDIE_PLAYBACK_ACTION_WAIT;
}

void roundie_pcm_to_stereo(const int16_t *input, uint32_t frames, uint8_t channels, uint32_t gain_q15,
                           int16_t *stereo)
{
    for (uint32_t index = 0; index < frames; index++) {
        int64_t left = (int64_t)input[index * channels] * gain_q15 >> 15;
        int64_t right = channels == 2 ? (int64_t)input[index * channels + 1] * gain_q15 >> 15 : left;
        left = left > 32767 ? 32767 : (left < -32768 ? -32768 : left);
        right = right > 32767 ? 32767 : (right < -32768 ? -32768 : right);
        stereo[2U * index] = (int16_t)left;
        stereo[2U * index + 1U] = (int16_t)right;
    }
}
