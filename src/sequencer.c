#include <string.h>
#include "driver/gptimer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "midi.h"
#include "sequencer.h"

static const char *TAG = "seq";

sq_pattern_t sq_pattern;

#define NO_NOTE 0xFF

static TaskHandle_t task;
static gptimer_handle_t timer;
static volatile bool playing, flush_req, restart_req;
static volatile int cur[SQ_TRACKS];
static uint32_t tick;
/* Sounding notes, to be ended by their gate (the channel is kept: it may be edited meanwhile) */
static uint8_t on_note[SQ_TRACKS][SQ_POLY], on_chan[SQ_TRACKS][SQ_POLY];
static uint32_t off_tick[SQ_TRACKS][SQ_POLY];

/* A pattern waiting to take over at the next pass of the longest track */
static sq_pattern_t staging;
static volatile bool pending;
static void set_alarm(void);

void sq_pattern_clear(sq_pattern_t *p)
{
    memset(p, 0, sizeof *p);
    p->bpm = 120;
    for (int t = 0; t < SQ_TRACKS; t++) {
        p->track[t].length = 16;
        p->track[t].channel = t;
        for (int i = 0; i < SQ_STEPS; i++) {
            p->track[t].steps[i].note[0] = 60;
            p->track[t].steps[i].gate = 50;
            p->track[t].steps[i].ratchet = 1;
        }
    }
}

void sq_load(const sq_pattern_t *p)
{
    pending = false;
    if (!playing) {
        memcpy(&sq_pattern, p, sizeof sq_pattern);
        return;
    }
    memcpy(&staging, p, sizeof staging);
    pending = true;
}

bool sq_pending(void)
{
    return pending;
}

static int tick_us(void)
{
    int bpm = sq_pattern.bpm;
    return 60000000 / (bpm * SQ_PPQ);
}

static bool IRAM_ATTR timer_cb(gptimer_handle_t t, const gptimer_alarm_event_data_t *ev, void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    return woken == pdTRUE;
}

static void all_off(void)
{
    for (int t = 0; t < SQ_TRACKS; t++) {
        for (int p = 0; p < SQ_POLY; p++) {
            if (on_note[t][p] != NO_NOTE) {
                midi_send(0x80 | on_chan[t][p], on_note[t][p], 0);
                on_note[t][p] = NO_NOTE;
            }
        }
    }
}

/* One note of a step at tick t. sublen = the ticks this hit may last, hold = the next step is tied to it, so it
 * lasts the whole step and is ended (one tick late, so the tied step can take over first) by the chain's end. */
static void hit(int tr, const sq_track_t *trk, const sq_step_t *s, uint32_t t, int sublen, bool hold, bool tied)
{
    for (int p = 0; p < SQ_POLY; p++) {
        int note = s->note[p] + trk->transpose;
        if (s->note[p] == NO_NOTE || note < 0 || note > 127) {
            continue;
        }
        int gate = s->gate < 1 ? 1 : s->gate;
        off_tick[tr][p] = t + (hold ? sublen + 1 : (sublen * gate + 99) / 100);
        if (tied && on_note[tr][p] != NO_NOTE) {
            if (on_note[tr][p] != note) {
                /* legato: the new note starts before the old one ends */
                midi_send(0x90 | on_chan[tr][p], note, s->vel);
                midi_send(0x80 | on_chan[tr][p], on_note[tr][p], 0);
                on_note[tr][p] = note;
            }
            continue;
        }
        if (on_note[tr][p] != NO_NOTE) {
            midi_send(0x80 | on_chan[tr][p], on_note[tr][p], 0);
        }
        on_chan[tr][p] = trk->channel & 15;
        on_note[tr][p] = note;
        midi_send(0x90 | on_chan[tr][p], note, s->vel);
    }
}

static void do_tick(void)
{
    uint32_t t = tick++;
    for (int tr = 0; tr < SQ_TRACKS; tr++) {
        for (int p = 0; p < SQ_POLY; p++) {
            if (on_note[tr][p] != NO_NOTE && (int32_t)(t - off_tick[tr][p]) >= 0) {
                midi_send(0x80 | on_chan[tr][p], on_note[tr][p], 0);
                on_note[tr][p] = NO_NOTE;
            }
        }
    }
    uint32_t phase = t % SQ_STEP_TICKS, stepno = t / SQ_STEP_TICKS;
    if (pending && !phase) {
        int maxlen = 1;
        for (int tr = 0; tr < SQ_TRACKS; tr++) {
            maxlen = sq_pattern.track[tr].length > maxlen ? sq_pattern.track[tr].length : maxlen;
        }
        if (stepno % maxlen == 0) {
            memcpy(&sq_pattern, &staging, sizeof sq_pattern);
            pending = false;
            set_alarm();  /* the new pattern's tempo */
        }
    }
    for (int tr = 0; tr < SQ_TRACKS; tr++) {
        const sq_track_t *trk = &sq_pattern.track[tr];
        int len = trk->length < 1 ? 1 : trk->length > SQ_STEPS ? SQ_STEPS : trk->length;
        int idx = stepno % len;
        if (!phase) {
            cur[tr] = idx;
        }
        const sq_step_t *s = &trk->steps[idx];
        if (!s->vel || trk->mute) {
            continue;
        }
        const sq_step_t *next = &trk->steps[(idx + 1) % len];
        bool hold = len > 1 && next->vel && next->tie;
        int r = s->ratchet > 1 && !s->tie && !hold ? (s->ratchet > 8 ? 8 : s->ratchet) : 1;
        for (int k = 0; k < r; k++) {
            if (phase == (uint32_t)(k * SQ_STEP_TICKS / r)) {
                hit(tr, trk, s, t, SQ_STEP_TICKS / r, hold, s->tie && !k);
            }
        }
    }
}

static void seq_task(void *arg)
{
    /* The timer's interrupt is allocated on the core that creates it: this one */
    gptimer_config_t cfg = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &timer));
    gptimer_event_callbacks_t cbs = {.on_alarm = timer_cb};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    xTaskNotifyGive((TaskHandle_t)arg);  /* sq_init waits for this */

    while (1) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (flush_req) {
            flush_req = false;
            all_off();
        }
        if (restart_req) {
            restart_req = false;
            all_off();
            tick = 0;
        }
        while (n-- && playing) {
            do_tick();
        }
    }
}

void sq_init(void)
{
    if (task) {
        return;
    }
    memset(on_note, NO_NOTE, sizeof on_note);
    sq_pattern_clear(&sq_pattern);
    xTaskCreatePinnedToCore(seq_task, "seq", 4096, xTaskGetCurrentTaskHandle(), configMAX_PRIORITIES - 3, &task, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "sequencer ready");
}

static void set_alarm(void)
{
    gptimer_alarm_config_t alarm = {
        .alarm_count = tick_us(),
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(timer, &alarm);
}

void sq_set_bpm(int bpm)
{
    sq_pattern.bpm = bpm < 20 ? 20 : bpm > 300 ? 300 : bpm;
    if (playing) {
        set_alarm();
    }
}

void sq_play(bool on)
{
    if (!task || on == playing) {
        return;
    }
    if (on) {
        for (int t = 0; t < SQ_TRACKS; t++) {
            cur[t] = -1;
        }
        tick = 0;
        playing = true;
        gptimer_set_raw_count(timer, 0);
        set_alarm();
        gptimer_start(timer);
        xTaskNotifyGive(task);  /* the first step now, not one tick late */
    } else {
        playing = false;
        gptimer_stop(timer);
        flush_req = true;
        xTaskNotifyGive(task);
    }
}

bool sq_playing(void)
{
    return playing;
}

void sq_restart(void)
{
    if (!playing) {
        return;
    }
    restart_req = true;
    xTaskNotifyGive(task);
}

int sq_position(int track)
{
    return playing ? cur[track] : -1;
}
