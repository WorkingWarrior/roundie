#include "roundie_audio.h"
#include <stdatomic.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "roundie_tone.h"

#define I2S_MCLK_GPIO GPIO_NUM_2
#define I2S_BCLK_GPIO GPIO_NUM_48
#define I2S_WS_GPIO GPIO_NUM_38
#define I2S_DOUT_GPIO GPIO_NUM_47
#define PA_CTRL_GPIO 9
#define CODEC_I2C_ADDRESS_7BIT 0x18
#define PROBE_TIMEOUT_MS 100
#define CHUNK_FRAMES 256U
#define TONE_AMPLITUDE 16000 /* half of full scale: headroom for the codec and the amplifier */
#define TASK_STACK 4096
#define TASK_PRIORITY 3
#define TONE_MAX_MS 5000U

static const char *TAG = "audio";

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t volume_percent;
} tone_request_t;

static esp_codec_dev_handle_t device;
static QueueHandle_t requests;
static roundie_audio_status_t status;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic bool busy;
static _Atomic bool stop_requested;

static esp_err_t fail(const char *step, esp_err_t error)
{
    portENTER_CRITICAL(&status_lock);
    status.init_error = error;
    status.init_step = step;
    portEXIT_CRITICAL(&status_lock);
    ESP_LOGE(TAG, "%s: %s", step, esp_err_to_name(error));
    return error;
}

static void count_error(void)
{
    portENTER_CRITICAL(&status_lock);
    status.errors++;
    portEXIT_CRITICAL(&status_lock);
}

static void play(const tone_request_t *request)
{
    uint8_t volume = (uint8_t)((uint32_t)request->volume_percent * ROUNDIE_AUDIO_MAX_PERCENT / 100U);
    portENTER_CRITICAL(&status_lock);
    status.playing = true;
    status.last_volume = volume;
    portEXIT_CRITICAL(&status_lock);

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = ROUNDIE_AUDIO_SAMPLE_RATE,
    };
    /* Opening the codec also switches the amplifier on and enables I2S. */
    if (esp_codec_dev_open(device, &format) != ESP_CODEC_DEV_OK) {
        count_error();
    } else {
        if (esp_codec_dev_set_out_vol(device, (float)volume) != ESP_CODEC_DEV_OK) {
            count_error();
        }
        roundie_tone_t tone;
        static int16_t chunk[2U * CHUNK_FRAMES];
        if (roundie_tone_init(&tone, request->frequency_hz, ROUNDIE_AUDIO_SAMPLE_RATE, request->duration_ms,
                              TONE_AMPLITUDE) != 0) {
            while (!atomic_load(&stop_requested)) {
                uint32_t frames = roundie_tone_fill(&tone, chunk, CHUNK_FRAMES);
                if (frames == 0) {
                    break;
                }
                if (esp_codec_dev_write(device, chunk, (int)(frames * 2U * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
                    count_error();
                    break;
                }
            }
            /* A little silence keeps the end of the tone from being cut by the close. */
            memset(chunk, 0, sizeof(chunk));
            esp_codec_dev_write(device, chunk, (int)sizeof(chunk));
        }
        esp_codec_dev_close(device);
    }
    portENTER_CRITICAL(&status_lock);
    status.playing = false;
    status.plays++;
    portEXIT_CRITICAL(&status_lock);
}

static void audio_task(void *argument)
{
    (void)argument;
    tone_request_t request;
    for (;;) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE) {
            play(&request);
            atomic_store(&stop_requested, false);
            atomic_store(&busy, false);
        }
    }
}

esp_err_t roundie_audio_init(i2c_master_bus_handle_t bus)
{
    status.init_step = "start";
    if (bus == NULL) {
        return fail("i2c bus", ESP_ERR_INVALID_ARG);
    }
    /* Identity first, on a temporary device: a missing codec is reported, not fatal. */
    esp_err_t probe = i2c_master_probe(bus, CODEC_I2C_ADDRESS_7BIT, PROBE_TIMEOUT_MS);
    status.codec_ack = probe == ESP_OK;
    if (status.codec_ack) {
        const i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CODEC_I2C_ADDRESS_7BIT,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t identity;
        if (i2c_master_bus_add_device(bus, &device_config, &identity) == ESP_OK) {
            for (uint8_t index = 0; index < 3; index++) {
                uint8_t reg = (uint8_t)(0xFD + index);
                i2c_master_transmit_receive(identity, &reg, 1, &status.codec_id[index], 1, PROBE_TIMEOUT_MS);
            }
            i2c_master_bus_rm_device(identity);
        }
    } else {
        return fail("codec not found", probe);
    }

    i2s_chan_handle_t transmit = NULL;
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true; /* silence, not a repeated buffer, if the feed stops */
    esp_err_t error = i2s_new_channel(&channel_config, &transmit, NULL);
    if (error != ESP_OK) {
        return fail("i2s channel", error);
    }
    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ROUNDIE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    error = i2s_channel_init_std_mode(transmit, &standard_config);
    if (error != ESP_OK) {
        i2s_del_channel(transmit);
        return fail("i2s init", error);
    }

    audio_codec_i2c_cfg_t i2c_config = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c_config);
    audio_codec_i2s_cfg_t i2s_config = {.port = I2S_NUM_0, .tx_handle = transmit, .rx_handle = NULL};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_config);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    if (control == NULL || data == NULL || gpio == NULL) {
        i2s_del_channel(transmit);
        return fail("codec interfaces", ESP_ERR_NO_MEM);
    }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin = PA_CTRL_GPIO,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0F, .codec_dac_voltage = 3.3F},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&codec_config);
    if (codec == NULL) {
        i2s_del_channel(transmit);
        return fail("es8311", ESP_FAIL);
    }
    esp_codec_dev_cfg_t device_config = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data};
    device = esp_codec_dev_new(&device_config);
    if (device == NULL) {
        i2s_del_channel(transmit);
        return fail("codec device", ESP_FAIL);
    }
    requests = xQueueCreate(1, sizeof(tone_request_t));
    if (requests == NULL || xTaskCreate(audio_task, "audio", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return fail("audio task", ESP_ERR_NO_MEM);
    }
    portENTER_CRITICAL(&status_lock);
    status.initialized = true;
    status.init_error = ESP_OK;
    status.init_step = "ok";
    portEXIT_CRITICAL(&status_lock);
    ESP_LOGI(TAG, "ES8311 id %02x %02x %02x, amplifier on GPIO%d, cap %u%%", status.codec_id[0], status.codec_id[1],
             status.codec_id[2], PA_CTRL_GPIO, ROUNDIE_AUDIO_MAX_PERCENT);
    return ESP_OK;
}

esp_err_t roundie_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    if (!status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz < ROUNDIE_TONE_MIN_HZ || frequency_hz > ROUNDIE_TONE_MAX_HZ || duration_ms < 10U ||
        duration_ms > TONE_MAX_MS || volume_percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&stop_requested, false);
    const tone_request_t request = {frequency_hz, duration_ms, volume_percent};
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        atomic_store(&busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void roundie_audio_stop(void)
{
    atomic_store(&stop_requested, true);
}

bool roundie_audio_busy(void)
{
    return atomic_load(&busy);
}

void roundie_audio_status(roundie_audio_status_t *out)
{
    portENTER_CRITICAL(&status_lock);
    *out = status;
    portEXIT_CRITICAL(&status_lock);
}
