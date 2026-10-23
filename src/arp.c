#include <string.h>
#include "arp.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "midi.h"
#include "sequencer.h"

arp_cfg_t arp_cfg;
const char *const arp_mode_names[ARP_MODES] = {"UP", "DOWN", "UP-DOWN", "DOWN-UP", "ORDER", "RANDOM", "CHORD"};
const char *const arp_rate_names[ARP_RATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const uint8_t rate_ticks[ARP_RATES] = {96, 48, 32, 24, 16, 12};

#define MAX_SOUNDING 64
#define NO_NOTE 0xFF

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
/* Written by the UI under the lock, read by the clock task */
#define SLOTS 32  /* 0-15 the pads, 16-31 notes from outside */
static uint8_t pad_notes[SLOTS][4], pad_count[SLOTS], pad_chan[SLOTS], pad_vel[SLOTS];
static uint32_t pad_order[SLOTS], order_ctr;
static bool pad_held[SLOTS], pad_down[SLOTS];
static uint8_t ext_slot[128];  /* the slot an outside note holds, 0xFF = none */   /* held = playing (latched pads stay held after they go up) */
static volatile bool flush_req, paused, retrig_req;

/* Clock task only */
static struct { uint8_t note, chan; uint32_t off; } sounding[MAX_SOUNDING];
static int sounding_n;
static uint32_t pos;
static bool last_src;
static volatile int shown_step;

void arp_defaults(void)
{
    memset(ext_slot, 0xFF, sizeof ext_slot);
    arp_cfg = (arp_cfg_t){.on = false, .mode = ARP_UP, .rate = 3, .gate = 60, .octaves = 1, .swing = 50, .vel = 0};
}

static void forget_pads(bool only_released)
{
    for (int p = 0; p < SLOTS; p++) {
        if (!only_released || !pad_down[p]) {
            pad_held[p] = false;
        }
    }
}

void arp_changed(void)
{
    taskENTER_CRITICAL(&mux);
    if (!arp_cfg.on) {
        forget_pads(false);
        memset(ext_slot, 0xFF, sizeof ext_slot);
        flush_req = true;
    } else if (!arp_cfg.latch) {
        forget_pads(true);
    }
    taskEXIT_CRITICAL(&mux);
    sq_clock_wake();
}

bool arp_pad_down(int pad, const uint8_t *notes, int count, int channel, int vel)
{
    if (!arp_cfg.on || count < 1) {
        return false;
    }
    taskENTER_CRITICAL(&mux);
    bool any_down = false;
    for (int p = 0; p < SLOTS; p++) {
        any_down |= pad_down[p];
    }
    if (!any_down) {
        forget_pads(false);  /* a new group replaces the latched one */
        retrig_req = true;
    } else if (arp_cfg.retrig) {
        retrig_req = true;
    }
    pad_count[pad] = count > 4 ? 4 : count;
    memcpy(pad_notes[pad], notes, pad_count[pad]);
    pad_chan[pad] = channel & 15;
    pad_vel[pad] = vel < 1 ? 1 : vel > 127 ? 127 : vel;
    pad_order[pad] = ++order_ctr;
    pad_held[pad] = pad_down[pad] = true;
    taskEXIT_CRITICAL(&mux);
    sq_clock_wake();
    return true;
}

void arp_pad_up(int pad)
{
    taskENTER_CRITICAL(&mux);
    pad_down[pad] = false;
    if (!arp_cfg.latch) {
        pad_held[pad] = false;
    }
    taskEXIT_CRITICAL(&mux);
}

bool arp_ext_down(int note, int channel, int vel)
{
    if (!arp_cfg.on) {
        return false;
    }
    int slot = ext_slot[note & 127];
    if (slot == 0xFF) {
        for (int p = 16; p < SLOTS && slot == 0xFF; p++) {
            if (!pad_held[p] && !pad_down[p]) {
                slot = p;
            }
        }
    }
    if (slot == 0xFF) {
        return true;  /* all the slots are in use: the note is dropped */
    }
    ext_slot[note & 127] = slot;
    uint8_t n = note & 127;
    return arp_pad_down(slot, &n, 1, channel, vel);
}

bool arp_ext_up(int note)
{
    int slot = ext_slot[note & 127];
    if (slot == 0xFF) {
        return false;
    }
    ext_slot[note & 127] = 0xFF;
    arp_pad_up(slot);
    return true;
}

void arp_panic(void)
{
    taskENTER_CRITICAL(&mux);
    forget_pads(false);
    memset(pad_down, 0, sizeof pad_down);
    memset(ext_slot, 0xFF, sizeof ext_slot);
    flush_req = true;
    taskEXIT_CRITICAL(&mux);
    sq_clock_wake();
}

void arp_pause(bool p)
{
    if (p != paused) {
        paused = p;
        if (p) {
            flush_req = true;
            sq_clock_wake();
        }
    }
}

void arp_silence(void)
{
    for (int i = 0; i < sounding_n; i++) {
        midi_send(0x80 | sounding[i].chan, sounding[i].note, 0);
    }
    sounding_n = 0;
}

void arp_service(void)
{
    if (flush_req) {
        flush_req = false;
        arp_silence();
        shown_step = -1;
    }
}

bool arp_active(void)
{
    if (sounding_n) {
        return true;
    }
    bool any = false;
    taskENTER_CRITICAL(&mux);
    for (int p = 0; p < SLOTS; p++) {
        any |= pad_held[p];
    }
    taskEXIT_CRITICAL(&mux);
    return any && arp_cfg.on && !paused;
}

typedef struct {
    uint8_t note, chan, vel;
    uint32_t order;
} ent_t;

/* Steps in one pass of the pattern, and which of the total (held notes x octaves) is played at step k of it */
static int period_of(int mode, int total, int oct)
{
    return mode == ARP_UPDN || mode == ARP_DNUP ? (total > 1 ? 2 * total - 2 : 1) : mode == ARP_CHORD ? oct : total;
}

static int index_at(int mode, int k, int total)
{
    int up = k < total ? k : 2 * total - 2 - k;
    return mode == ARP_DOWN ? total - 1 - k : mode == ARP_UPDN ? up : mode == ARP_DNUP ? total - 1 - up : k;
}

static void play(int note, int chan, int vel, uint32_t off)
{
    if (note > 127 || sounding_n >= MAX_SOUNDING) {
        return;
    }
    midi_send(0x90 | chan, note, vel);
    sounding[sounding_n].note = note;
    sounding[sounding_n].chan = chan;
    sounding[sounding_n].off = off;
    sounding_n++;
}

/* The held notes, one entry each, sorted by pitch (by press order in ORDER mode) */
static int gather(ent_t *e, bool by_order)
{
    int n = 0;
    taskENTER_CRITICAL(&mux);
    for (int p = 0; p < SLOTS; p++) {
        if (!pad_held[p]) {
            continue;
        }
        for (int k = 0; k < pad_count[p]; k++) {
            bool dup = false;
            for (int i = 0; i < n; i++) {
                dup |= e[i].note == pad_notes[p][k] && e[i].chan == pad_chan[p];
            }
            if (!dup && n < 64) {
                int vel = pad_vel[p];
                e[n++] = (ent_t){pad_notes[p][k], pad_chan[p], vel, pad_order[p] * 8 + k};
            }
        }
    }
    taskEXIT_CRITICAL(&mux);
    for (int i = 1; i < n; i++) {
        ent_t x = e[i];
        int j = i - 1;
        while (j >= 0 && (by_order ? e[j].order > x.order : e[j].note > x.note)) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = x;
    }
    return n;
}

void arp_tick(uint32_t t, bool from_sequencer)
{
    if (from_sequencer != last_src) {
        last_src = from_sequencer;  /* the tick count jumped: the gates in flight are meaningless */
        arp_silence();
    }
    for (int i = 0; i < sounding_n;) {
        if ((int32_t)(t - sounding[i].off) >= 0) {
            midi_send(0x80 | sounding[i].chan, sounding[i].note, 0);
            sounding[i] = sounding[--sounding_n];
        } else {
            i++;
        }
    }
    if (!arp_cfg.on || paused) {
        return;
    }
    uint32_t len = rate_ticks[arp_cfg.rate < ARP_RATES ? arp_cfg.rate : 3];
    uint32_t sw = arp_cfg.swing > 50 ? (len * (arp_cfg.swing > 75 ? 25 : arp_cfg.swing - 50)) / 50 : 0;
    if (t % len != ((t / len) & 1 ? sw : 0)) {
        return;
    }
    ent_t e[64];
    int ne = gather(e, arp_cfg.mode == ARP_ORDER);
    if (!ne) {
        return;
    }
    if (retrig_req) {
        retrig_req = false;
        pos = 0;
    }
    int oct = arp_cfg.octaves < 1 ? 1 : arp_cfg.octaves > 4 ? 4 : arp_cfg.octaves;
    int gate = arp_cfg.gate < 5 ? 5 : arp_cfg.gate > 100 ? 100 : arp_cfg.gate;
    uint32_t off = t + (len * gate + 99) / 100;
    arp_silence();  /* a gate of 100 % touches the next note */
    int total = ne * oct, period = period_of(arp_cfg.mode, total, oct), k = pos % period;
    int i = arp_cfg.mode == ARP_RANDOM ? (int)(esp_random() % total) : index_at(arp_cfg.mode, k, total);
    int vel_fixed = arp_cfg.vel;
    if (arp_cfg.mode == ARP_CHORD) {
        for (int n = 0; n < ne; n++) {
            play(e[n].note + 12 * k, e[n].chan, vel_fixed ? vel_fixed : e[n].vel, off);
        }
    } else {
        play(e[i % ne].note + 12 * (i / ne), e[i % ne].chan, vel_fixed ? vel_fixed : e[i % ne].vel, off);
    }
    shown_step = k;
    pos++;
}

int arp_step(void)
{
    return arp_cfg.on && arp_active() ? shown_step : -1;
}

int arp_cycle(uint8_t *notes, int max)
{
    ent_t e[64];
    int ne = gather(e, arp_cfg.mode == ARP_ORDER);
    if (!ne) {
        return 0;
    }
    int oct = arp_cfg.octaves < 1 ? 1 : arp_cfg.octaves > 4 ? 4 : arp_cfg.octaves;
    int total = ne * oct, period = period_of(arp_cfg.mode, total, oct), n = period < max ? period : max;
    for (int k = 0; k < n; k++) {
        int i = arp_cfg.mode == ARP_RANDOM ? k : index_at(arp_cfg.mode, k, total);
        int note = arp_cfg.mode == ARP_CHORD ? e[0].note + 12 * k : e[i % ne].note + 12 * (i / ne);
        notes[k] = note > 127 ? 127 : note;
    }
    return n;
}

int arp_held(uint8_t *notes, int max)
{
    ent_t e[64];
    int ne = gather(e, arp_cfg.mode == ARP_ORDER);
    ne = ne < max ? ne : max;
    for (int i = 0; i < ne; i++) {
        notes[i] = e[i].note;
    }
    return ne;
}
