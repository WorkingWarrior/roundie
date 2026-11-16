/* Speaker of the Linux desktop (docs/native-apps.md, N8): the roundie_audio.h
 * stream contract on an SDL3 playback stream, in place of the board's ES8311.
 *
 * The playback policy (prebuffer, underrun, pause, drain), the ring and the PCM
 * conversion are the firmware's (components/roundie_audio/stream.c). Like the
 * firmware's audio task, one thread owns the output: it plays a chunk whenever
 * SDL has less than LEAD_CHUNKS of them queued, which stands in for the codec's
 * blocking I2S write. Tones and the melody are firmware diagnostics and are
 * not provided here.
 *
 * Virtual hardware lab (vhw.h): an unavailable output device fails the start
 * and every later write, as a codec or I2S failure does on the board; a
 * requested underrun drops what the ring holds. */

#include "roundie_audio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "esp_log.h"
#include "roundie_platform.h"
#include "roundie_stream.h"
#include "vhw.h"

#define CHUNK_FRAMES 256U
#define LEAD_CHUNKS 2U          /* queued in SDL before the thread waits, about 32 ms at 16 kHz */
#define POLL_MS 5U
#define DRAIN_WAIT_MS 500U      /* bound on playing out what SDL holds at a drained close */

static const char *TAG = "audio";

typedef struct {
    bool open;
    bool closing;
    bool drain;
    uint32_t owner;
    uint32_t generation;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t gain_q15;
    uint8_t *storage;
    roundie_ring_t ring;
    roundie_playback_t playback;
    bool device_open; /* the SDL stream is playing this stream's format */
} stream_t;

static SDL_AudioStream *output;
static pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER;
static stream_t stream;
static uint32_t generation_counter;
static uint32_t total_underruns;
static uint32_t total_pauses;
static roundie_lock_t status_lock = ROUNDIE_LOCK_INITIALIZER;
static roundie_audio_status_t status;
static atomic_bool busy;
static _Atomic uint8_t master_percent = ROUNDIE_AUDIO_MASTER_DEFAULT;

static void count_error(void)
{
    roundie_lock(&status_lock);
    status.errors++;
    roundie_unlock(&status_lock);
}

static bool valid_rate(uint32_t rate)
{
    return rate == 8000U || rate == 16000U || rate == 24000U || rate == 32000U || rate == 48000U;
}

static uint8_t volume_for_master(void)
{
    return (uint8_t)((uint32_t)atomic_load(&master_percent) * ROUNDIE_AUDIO_MAX_PERCENT / 100U);
}

/* Audio thread only. Plays out (drain) or drops what SDL holds, then frees the stream. */
static void stream_release(bool play_out)
{
    if (stream.device_open) {
        if (play_out) {
            SDL_FlushAudioStream(output);
            for (unsigned waited = 0; waited < DRAIN_WAIT_MS && SDL_GetAudioStreamQueued(output) > 0;
                 waited += POLL_MS) {
                roundie_sleep_ms(POLL_MS);
            }
        }
        SDL_PauseAudioStreamDevice(output);
        SDL_ClearAudioStream(output);
        stream.device_open = false;
    }
    pthread_mutex_lock(&stream_mutex);
    total_underruns += stream.playback.underruns;
    total_pauses += stream.playback.pauses;
    free(stream.storage);
    stream.storage = NULL;
    stream.open = false;
    stream.closing = false;
    stream.drain = false;
    stream.owner = 0;
    roundie_lock(&status_lock);
    status.playing = false;
    status.plays++;
    roundie_unlock(&status_lock);
    atomic_store(&busy, false);
    pthread_mutex_unlock(&stream_mutex);
}

static bool output_unavailable(void)
{
    vhw_state_t state;
    vhw_get(&state, roundie_time_us());
    return state.audio == VHW_AUDIO_UNAVAILABLE;
}

/* Audio thread only: decides and plays chunks until SDL holds enough or it has to wait. */
static void stream_service(void)
{
    static int16_t stereo[2U * CHUNK_FRAMES];
    static int16_t pcm[2U * CHUNK_FRAMES];
    const size_t chunk_bytes = (size_t)CHUNK_FRAMES * stream.channels * sizeof(int16_t);
    for (;;) {
        if (stream.device_open && output_unavailable()) {
            ESP_LOGE(TAG, "write: output device unavailable (lab)");
            count_error();
            stream_release(false);
            return;
        }
        pthread_mutex_lock(&stream_mutex);
        bool closing = stream.closing;
        bool drain = stream.drain;
        size_t used = roundie_ring_used(&stream.ring);
        pthread_mutex_unlock(&stream_mutex);
        if (closing && !drain) {
            stream_release(false);
            return;
        }
        if (stream.device_open && SDL_GetAudioStreamQueued(output) >= (int)(LEAD_CHUNKS * sizeof(stereo))) {
            return; /* the "codec" is busy: come back after a poll */
        }
        if (vhw_take_underrun()) {
            /* The ring runs dry before this decision, as when the producer falls behind. */
            pthread_mutex_lock(&stream_mutex);
            roundie_ring_clear(&stream.ring);
            pthread_mutex_unlock(&stream_mutex);
            used = 0;
            ESP_LOGW(TAG, "lab: buffered audio dropped");
        }
        stream.playback.drain = closing && drain;
        roundie_playback_action_t action = roundie_playback_step(&stream.playback, used, chunk_bytes,
                                                                 roundie_time_us());
        if (action == ROUNDIE_PLAYBACK_ACTION_START) {
            SDL_AudioSpec format = {SDL_AUDIO_S16, 2, (int)stream.sample_rate};
            uint8_t volume = volume_for_master();
            if (output_unavailable()) {
                ESP_LOGE(TAG, "open: output device unavailable (lab)");
                count_error();
                stream_release(false);
                return;
            }
            if (!SDL_SetAudioStreamFormat(output, &format, NULL) ||
                !SDL_SetAudioStreamGain(output, (float)volume / 100.0f) || !SDL_ResumeAudioStreamDevice(output)) {
                ESP_LOGE(TAG, "open: %s", SDL_GetError());
                count_error();
                stream_release(false);
                return;
            }
            stream.device_open = true;
            roundie_lock(&status_lock);
            status.playing = true;
            status.last_volume = volume;
            roundie_unlock(&status_lock);
            action = ROUNDIE_PLAYBACK_ACTION_PLAY;
        }
        if (action == ROUNDIE_PLAYBACK_ACTION_PLAY) {
            pthread_mutex_lock(&stream_mutex); /* unlike the firmware ring, not shared lock-free here */
            size_t got = roundie_ring_read(&stream.ring, pcm, chunk_bytes);
            pthread_mutex_unlock(&stream_mutex);
            uint32_t frames = (uint32_t)(got / ((size_t)stream.channels * sizeof(int16_t)));
            roundie_pcm_to_stereo(pcm, frames, stream.channels, stream.gain_q15, stereo);
            if (frames > 0 && !SDL_PutAudioStreamData(output, stereo, (int)(frames * 2U * sizeof(int16_t)))) {
                count_error();
                stream_release(false);
                return;
            }
        } else if (action == ROUNDIE_PLAYBACK_ACTION_SILENCE) {
            memset(stereo, 0, sizeof(stereo));
            if (!SDL_PutAudioStreamData(output, stereo, (int)sizeof(stereo))) {
                count_error();
                stream_release(false);
                return;
            }
        } else if (action == ROUNDIE_PLAYBACK_ACTION_PAUSE) {
            SDL_PauseAudioStreamDevice(output);
            SDL_ClearAudioStream(output);
            stream.device_open = false;
            roundie_lock(&status_lock);
            status.playing = false;
            roundie_unlock(&status_lock);
        } else if (action == ROUNDIE_PLAYBACK_ACTION_FINISH) {
            stream_release(true);
            return;
        } else {
            return; /* WAIT */
        }
    }
}

static void *audio_thread(void *argument)
{
    (void)argument;
    for (;;) {
        pthread_mutex_lock(&stream_mutex);
        bool open = stream.open;
        pthread_mutex_unlock(&stream_mutex);
        if (open) {
            stream_service();
        }
        roundie_sleep_ms(POLL_MS);
    }
    return NULL;
}

esp_err_t roundie_audio_init(roundie_board_t *board)
{
    (void)board;
    roundie_lock(&status_lock);
    status.init_step = "start";
    roundie_unlock(&status_lock);
    SDL_AudioSpec format = {SDL_AUDIO_S16, 2, (int)ROUNDIE_AUDIO_SAMPLE_RATE};
    if (output_unavailable()) {
        ESP_LOGE(TAG, "speaker: output device unavailable (lab)");
        roundie_lock(&status_lock);
        status.init_error = ESP_FAIL;
        status.init_step = "speaker (lab)";
        roundie_unlock(&status_lock);
        return ESP_FAIL;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        ESP_LOGE(TAG, "SDL audio: %s", SDL_GetError());
        roundie_lock(&status_lock);
        status.init_error = ESP_FAIL;
        status.init_step = "SDL audio";
        roundie_unlock(&status_lock);
        return ESP_FAIL;
    }
    output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &format, NULL, NULL);
    pthread_t thread;
    if (output == NULL || pthread_create(&thread, NULL, audio_thread, NULL) != 0) {
        ESP_LOGE(TAG, "SDL playback stream: %s", SDL_GetError());
        roundie_lock(&status_lock);
        status.init_error = ESP_FAIL;
        status.init_step = "SDL playback stream";
        roundie_unlock(&status_lock);
        return ESP_FAIL;
    }
    pthread_detach(thread);
    roundie_lock(&status_lock);
    status.initialized = true;
    status.init_error = ESP_OK;
    status.init_step = "ok";
    roundie_unlock(&status_lock);
    ESP_LOGI(TAG, "Speaker ready on SDL driver %s, cap %u%%", SDL_GetCurrentAudioDriver(), ROUNDIE_AUDIO_MAX_PERCENT);
    return ESP_OK;
}

bool roundie_audio_busy(void)
{
    return atomic_load(&busy);
}

void roundie_audio_status(roundie_audio_status_t *out)
{
    roundie_lock(&status_lock);
    *out = status;
    roundie_unlock(&status_lock);
    pthread_mutex_lock(&stream_mutex);
    out->stream_open = stream.open;
    out->stream_phase = (uint8_t)stream.playback.phase;
    out->ring_used = stream.open ? (uint32_t)roundie_ring_used(&stream.ring) : 0;
    out->underruns = total_underruns + (stream.open ? stream.playback.underruns : 0);
    out->pauses = total_pauses + (stream.open ? stream.playback.pauses : 0);
    pthread_mutex_unlock(&stream_mutex);
    out->master_percent = atomic_load(&master_percent);
}

void roundie_audio_set_master(uint8_t percent)
{
    atomic_store(&master_percent, percent > 100U ? 100U : percent);
}

roundie_audio_stream_t roundie_audio_stream_open(uint32_t owner, uint32_t sample_rate, uint8_t channels,
                                                 uint8_t volume_percent)
{
    roundie_audio_status_t current;
    roundie_audio_status(&current);
    if (!current.initialized || !valid_rate(sample_rate) || (channels != 1U && channels != 2U) ||
        volume_percent > 100U) {
        return 0;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return 0;
    }
    uint8_t *storage = malloc(ROUNDIE_AUDIO_RING_BYTES);
    if (storage == NULL) {
        atomic_store(&busy, false);
        return 0;
    }
    pthread_mutex_lock(&stream_mutex);
    stream.storage = storage;
    roundie_ring_init(&stream.ring, storage, ROUNDIE_AUDIO_RING_BYTES);
    stream.owner = owner;
    stream.sample_rate = sample_rate;
    stream.channels = channels;
    stream.gain_q15 = (uint32_t)volume_percent * 32768U / 100U;
    stream.closing = false;
    stream.drain = false;
    stream.device_open = false;
    size_t prebuffer = (size_t)sample_rate * channels * sizeof(int16_t) * ROUNDIE_AUDIO_PREBUFFER_MS / 1000U;
    if (prebuffer > ROUNDIE_AUDIO_RING_BYTES / 2U) {
        prebuffer = ROUNDIE_AUDIO_RING_BYTES / 2U;
    }
    roundie_playback_init(&stream.playback, prebuffer, (int64_t)ROUNDIE_AUDIO_PAUSE_AFTER_MS * 1000);
    stream.generation = ++generation_counter;
    if (stream.generation == 0) {
        stream.generation = ++generation_counter;
    }
    stream.open = true;
    uint32_t handle = stream.generation;
    pthread_mutex_unlock(&stream_mutex);
    return handle;
}

int32_t roundie_audio_stream_write(roundie_audio_stream_t handle, const void *pcm, uint32_t bytes)
{
    if (handle == 0 || pcm == NULL) {
        return -1;
    }
    pthread_mutex_lock(&stream_mutex);
    int32_t accepted = -1;
    if (stream.open && !stream.closing && stream.generation == handle) {
        uint32_t frame_bytes = (uint32_t)stream.channels * (uint32_t)sizeof(int16_t);
        uint32_t whole = bytes - bytes % frame_bytes;
        accepted = (int32_t)roundie_ring_write(&stream.ring, pcm, whole);
    }
    pthread_mutex_unlock(&stream_mutex);
    return accepted;
}

int32_t roundie_audio_stream_writable(roundie_audio_stream_t handle)
{
    if (handle == 0) {
        return -1;
    }
    pthread_mutex_lock(&stream_mutex);
    int32_t free_bytes = -1;
    if (stream.open && !stream.closing && stream.generation == handle) {
        uint32_t frame_bytes = (uint32_t)stream.channels * (uint32_t)sizeof(int16_t);
        uint32_t space = (uint32_t)roundie_ring_free(&stream.ring);
        free_bytes = (int32_t)(space - space % frame_bytes);
    }
    pthread_mutex_unlock(&stream_mutex);
    return free_bytes;
}

void roundie_audio_stream_close(roundie_audio_stream_t handle, bool drain)
{
    if (handle == 0) {
        return;
    }
    pthread_mutex_lock(&stream_mutex);
    if (stream.open && stream.generation == handle) {
        stream.drain = drain;
        stream.closing = true;
    }
    pthread_mutex_unlock(&stream_mutex);
}

bool roundie_audio_release_owner(uint32_t owner, uint32_t timeout_ms)
{
    pthread_mutex_lock(&stream_mutex);
    bool mine = stream.open && stream.owner == owner;
    if (mine) {
        stream.drain = false;
        stream.closing = true;
    }
    pthread_mutex_unlock(&stream_mutex);
    int64_t deadline = roundie_time_us() + (int64_t)timeout_ms * 1000;
    while (mine) {
        pthread_mutex_lock(&stream_mutex);
        bool still = stream.open && stream.owner == owner;
        pthread_mutex_unlock(&stream_mutex);
        if (!still) {
            return true;
        }
        if (roundie_time_us() > deadline) {
            return false;
        }
        roundie_sleep_ms(POLL_MS);
    }
    return true;
}
