#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Speaker output (docs/audio.md): the ES8311 codec and the NS4150B amplifier on
 * the shared I2C bus, fed by one I2S transmit channel. A system audio task owns
 * the codec and the amplifier; nothing else touches them. */

#define ROUNDIE_AUDIO_MAX_PERCENT 50U /* system cap on the codec output volume, an assumption */
#define ROUNDIE_AUDIO_SAMPLE_RATE 16000U

typedef struct {
    bool initialized;
    esp_err_t init_error;      /* first failure, ESP_OK when initialised */
    const char *init_step;     /* the step that failed, or "ok" */
    bool codec_ack;            /* the ES8311 answered at its I2C address */
    uint8_t codec_id[3];       /* ES8311 registers 0xFD, 0xFE, 0xFF */
    bool playing;
    uint32_t plays;            /* tones that ran to their end or were stopped */
    uint32_t errors;           /* codec open, volume or write failures */
    uint8_t last_volume;       /* codec volume of the last tone after the cap */
} roundie_audio_status_t;

/* Once, after the board's I2C bus exists; not in Safe Mode. Failure leaves audio
 * unavailable and does not affect anything else. */
esp_err_t roundie_audio_init(i2c_master_bus_handle_t bus);
/* A tone from the system. volume_percent 0..100 is a share of the system cap.
 * Refused while a tone plays or when audio is unavailable. */
esp_err_t roundie_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
/* Ends the tone in progress at its next chunk. */
void roundie_audio_stop(void);
bool roundie_audio_busy(void);
void roundie_audio_status(roundie_audio_status_t *out);
