/* Sound: a button plays a short folk tune ("Wlazł kotek na płotek") through
 * rep_audio_v1, and stops it when pressed again. The app generates 16 kHz mono
 * PCM itself (integer sine table, 5 ms fades per note) and feeds the system's
 * stream from a 50 ms timer: each tick it writes what fits. When the tune is
 * written it closes the stream with drain, so the buffered end still plays.
 * Without working audio the button is disabled. No LVGL access. */
#include "rep/rep_app_v3.h"
#include "rep/rep_audio_v1.h"
#include <stdbool.h>
REP3_APP_METADATA;

#define SAMPLE_RATE 16000U
#define TICK_MS 50U
#define AMPLITUDE 16000         /* half of full scale */
#define FADE_SAMPLES 80U        /* 5 ms at 16 kHz */
#define GAP_SAMPLES 320U        /* 20 ms of silence after each note */
#define CHUNK_SAMPLES 1024U
#define BUTTON_COLOR 0x30d158
#define STOP_COLOR 0xff453a

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
} note_t;

/* G E E F D D C E G | G E E F D D C E C, one octave up for a small speaker. */
static const note_t tune[] = {
    {784, 300}, {659, 300}, {659, 300}, {698, 300}, {587, 300}, {587, 300}, {523, 300}, {659, 300}, {784, 600},
    {784, 300}, {659, 300}, {659, 300}, {698, 300}, {587, 300}, {587, 300}, {523, 300}, {659, 300}, {523, 600},
};
#define TUNE_NOTES (sizeof(tune) / sizeof(tune[0]))

static const int16_t sine[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
    25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
    28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
    15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
    -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
    -3212, -2410, -1608, -804,
};

static const rep_api_v3_t *api;
static const rep_audio_v1_t *audio;
static uint32_t view;
static uint32_t title_label;
static uint32_t play_button;
static uint32_t timer;
static uint32_t stream;
/* Tune position: the current note, the samples written of it (tone, then gap)
 * and the sine phase (a 32-bit fraction of one period). */
static uint32_t note;
static uint32_t note_sample;
static uint32_t phase;
static int16_t pcm[CHUNK_SAMPLES]; /* app data: rep_audio_v1 refuses stack pointers */
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t view_widget = {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
static rep_ui_widget_v2_t title_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LABEL, 0, 60, 96, 240, 60, 8, "Dźwięk"},
    0, 0, 0, REP_UI_OPTION_FONT_LARGE};
static rep_ui_widget_t play_widget = {1, sizeof(rep_ui_widget_t), REP_UI_BUTTON, 0, 110, 196, 140, 56, 4, "Graj"};

static int32_t set(uint32_t instance, uint32_t widget, uint32_t property, uint32_t value)
{
    update.widget = widget;
    update.property = property;
    update.value = value;
    return api->ui_update(instance, &update);
}

static int32_t set_text(uint32_t instance, uint32_t widget, const char *text)
{
    uint32_t length = 0;
    while (text[length] != '\0' && length < REP_UI_TEXT_CAPACITY - 1U) {
        update.text[length] = text[length];
        length++;
    }
    update.text[length] = '\0';
    return set(instance, widget, REP_UI_TEXT, length);
}

static uint32_t note_samples(uint32_t index)
{
    return (uint32_t)tune[index].duration_ms * (SAMPLE_RATE / 1000U);
}

/* Phase step per sample: frequency * 2^32 / 16000, without 64-bit division. */
static uint32_t phase_step(uint32_t frequency_hz)
{
    return frequency_hz * 268435U + frequency_hz * 456U / 1000U;
}

/* Fills up to count samples of the tune; returns how many (0 at its end). */
static uint32_t render(uint32_t count)
{
    uint32_t done = 0;
    while (done < count && note < TUNE_NOTES) {
        uint32_t tone = note_samples(note);
        if (note_sample < tone) {
            uint32_t edge = note_sample < tone - note_sample ? note_sample : tone - note_sample;
            int32_t amplitude = edge < FADE_SAMPLES ? AMPLITUDE * (int32_t)edge / (int32_t)FADE_SAMPLES : AMPLITUDE;
            pcm[done] = (int16_t)((sine[phase >> 24] * amplitude) >> 15);
            phase += phase_step(tune[note].frequency_hz);
        } else {
            pcm[done] = 0;
        }
        done++;
        note_sample++;
        if (note_sample == tone + GAP_SAMPLES) {
            note++;
            note_sample = 0;
            phase = 0;
        }
    }
    return done;
}

static int32_t show_playing(uint32_t instance, bool playing)
{
    if (set_text(instance, play_button, playing ? "Stop" : "Graj") != 0 ||
        set(instance, play_button, REP_UI_COLOR, playing ? STOP_COLOR : BUTTON_COLOR) != 0) {
        return -21;
    }
    return 0;
}

static int32_t finish(uint32_t instance, bool drain)
{
    if (stream != 0) {
        audio->close(instance, stream, drain ? 1U : 0U);
        stream = 0;
    }
    if (timer != 0) {
        api->timer_cancel(instance, timer);
        timer = 0;
    }
    return show_playing(instance, false);
}

/* Writes what fits now. At the tune's end, closes with drain. */
static int32_t feed(uint32_t instance)
{
    for (;;) {
        int32_t space = audio->writable(instance, stream);
        if (space < 0) {
            return finish(instance, false); /* the system closed the stream */
        }
        uint32_t samples = (uint32_t)space / sizeof(int16_t);
        if (samples > CHUNK_SAMPLES) {
            samples = CHUNK_SAMPLES;
        }
        if (samples == 0U) {
            return 0;
        }
        uint32_t made = render(samples);
        if (made == 0U) {
            return finish(instance, true);
        }
        if (audio->write(instance, stream, pcm, made * sizeof(int16_t)) != (int32_t)(made * sizeof(int16_t))) {
            return -31; /* writable said it fits */
        }
    }
}

static int32_t play(uint32_t instance)
{
    note = 0;
    note_sample = 0;
    phase = 0;
    stream = audio->open(instance, SAMPLE_RATE, 1, 100);
    if (stream == 0) {
        return 0; /* busy: a system sound or another stream; stay idle */
    }
    timer = api->timer_start(instance, TICK_MS);
    if (timer == 0) {
        return finish(instance, false);
    }
    int32_t result = show_playing(instance, true);
    return result != 0 ? result : feed(instance);
}

static int32_t initialize(const rep_context_v3_t *context)
{
    uint32_t instance = context->instance;
    timer = 0;
    stream = 0;
    audio = rep_audio_v1(REP_AUDIO_V1);
    if (audio != NULL && (audio->version != REP_AUDIO_V1 || audio->struct_size != sizeof(*audio))) {
        audio = NULL;
    }
    view = api->ui_create(instance, &view_widget);
    if (view == 0) {
        return -11;
    }
    title_widget.base.parent = view;
    play_widget.parent = view;
    title_label = api->ui_create(instance, &title_widget.base);
    play_button = api->ui_create(instance, &play_widget);
    if (title_label == 0 || play_button == 0) {
        return -12;
    }
    if (set(instance, view, REP_UI_COLOR, 0x000000) != 0 ||
        set(instance, title_label, REP_UI_COLOR, 0xffffff) != 0 ||
        set(instance, play_button, REP_UI_COLOR, BUTTON_COLOR) != 0) {
        return -13;
    }
    if (audio == NULL) {
        if (set_text(instance, title_label, "Brak dźwięku") != 0 ||
            set(instance, play_button, REP_UI_ENABLED, 0) != 0) {
            return -14;
        }
    }
    return 0;
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    uint32_t instance = context->instance;
    if (event->type == REP2_EVENT_TIMER && timer != 0 && event->value == timer && stream != 0) {
        return feed(instance);
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == play_button && audio != NULL) {
        return stream != 0 ? finish(instance, false) : play(instance);
    }
    return 0;
}

/* The system closes the stream at stop; forget the handle. */
static int32_t stop(const rep_context_v3_t *context)
{
    (void)context;
    stream = 0;
    timer = 0;
    return 0;
}

static int32_t quiet(const rep_context_v3_t *context)
{
    (void)context;
    return 0;
}

static const rep_descriptor_v3_t descriptor = {
    3, sizeof(descriptor), initialize, quiet, event, quiet, quiet, stop, quiet,
    REP3_CAP_UI | REP3_CAP_ROUND_UI | REP2_CAP_TIMER,
};

REP2_EXPORT const rep_descriptor_v3_t *rep_app_entry_v3(const rep_api_v3_t *table)
{
    api = rep_abi_v3(REP_ABI_V3);
    if (api != table || api == NULL || api->abi_version != 3 || api->struct_size != sizeof(*api) ||
        (api->capabilities & descriptor.required_capabilities) != descriptor.required_capabilities) {
        return NULL;
    }
    return &descriptor;
}
