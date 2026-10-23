#include <stdio.h>
#include <string.h>
#include "arp.h"
#include "arp_ui.h"
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "sequencer.h"
#include "synth.h"

#define COLUMN_WIDTH 64
#define COLUMN_INNER 63
#define HEADER_LINE_Y 11
#define NAME_CAP_Y 41
#define VALUE_CAP_Y 52
#define SETTINGS_FILE "/lfs/arp"
#define SETTINGS_VERSION 1
#define SAVE_DELAY_US 2000000

static const char *TAG = "arp";
static int acc[8];
static int64_t save_at;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void text_cap(int d, int x, int cap_y, const font_t *f, const char *s, bool on)
{
    display_text(d, x, cap_y - f->cap_top, f, s, on);
}

static void centered(int d, int x, int w, int cap_y, const font_t *f, const char *s, bool on)
{
    text_cap(d, x + (w - display_text_width(f, s)) / 2, cap_y, f, s, on);
}

static void touched(void)
{
    save_at = 1;  /* set to a real time by the next tick */
}

void arp_ui_init(void)
{
    arp_defaults();
    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (!f) {
        return;
    }
    uint8_t b[9];
    if (fread(b, 1, sizeof b, f) == sizeof b && b[0] == SETTINGS_VERSION) {
        arp_cfg.mode = b[1] < ARP_MODES ? b[1] : ARP_UP;
        arp_cfg.rate = b[2] < ARP_RATES ? b[2] : 3;
        arp_cfg.gate = clampi(b[3], 5, 100);
        arp_cfg.octaves = clampi(b[4], 1, 4);
        arp_cfg.swing = clampi(b[5], 50, 75);
        arp_cfg.vel = clampi(b[6], 0, 127);
        arp_cfg.latch = b[7] != 0;
        arp_cfg.retrig = b[8] != 0;
    }
    fclose(f);
}

static void save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) {
        ESP_LOGE(TAG, "can't write %s", SETTINGS_FILE);
        return;
    }
    uint8_t b[9] = {SETTINGS_VERSION, arp_cfg.mode, arp_cfg.rate, arp_cfg.gate, arp_cfg.octaves, arp_cfg.swing,
                    arp_cfg.vel, arp_cfg.latch, arp_cfg.retrig};
    fwrite(b, 1, sizeof b, f);
    fclose(f);
}

#define STRIP_Y 14       /* the picture above the knob texts: y 14-36 */
#define STRIP_H 22
#define REDRAW_US 50000  /* never faster (see CLAUDE.md) */
#define CYCLE_MAX 32

static void note_name(int note, char *buf, size_t len)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[note % 12], note / 12 - 1);
}

/* Left: the notes held, in playing order. Right: one pass of the pattern as bars (a note's height is its pitch),
 * the step playing now filled */
static uint32_t strip_sig = ~0u;

static void draw_strip(void)
{
    uint8_t notes[CYCLE_MAX];
    bool chord = arp_cfg.mode == ARP_CHORD;
    int n = chord ? arp_held(notes, CYCLE_MAX) : arp_cycle(notes, CYCLE_MAX), cur = arp_step();
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, STRIP_Y - 1, DISPLAY_USABLE_WIDTH, STRIP_H + 2, false);
    }
    if (!n) {
        display_text(0, 2, STRIP_Y + STRIP_H / 2 - 4, &font_tiny, "hold a pad to see the pattern", true);
        return;
    }
    int x = 2;
    for (int k = 0; k < n && k < 8; k++) {
        char b[8];
        note_name(notes[k], b, sizeof b);
        x = display_text(0, x, STRIP_Y + STRIP_H / 2 - 4, &font_tiny, b, true) + 5;
    }
    /* The bars of one pass: a chord has all its notes side by side in a group an octave (the group playing now is
     * filled), the other modes one bar a step. Centred on the right display; three bars take about a third of it,
     * more notes widen it until it fills the display, then the bars thin out. */
    int oct = chord ? arp_cfg.octaves : 1, bars = chord ? n * oct : n, lo = 127, hi = 0;
    uint8_t pitch[CYCLE_MAX * 4];
    for (int b = 0; b < bars; b++) {
        pitch[b] = chord ? notes[b % n] + 12 * (b / n) : notes[b];
        lo = pitch[b] < lo ? pitch[b] : lo;
        hi = pitch[b] > hi ? pitch[b] : hi;
    }
    const int group_gap = 8, max_slot = 28, avail = DISPLAY_USABLE_WIDTH - 8;
    int slot = (avail - (oct - 1) * group_gap) / bars;
    slot = slot > max_slot ? max_slot : slot < 2 ? 2 : slot;
    int gap = slot >= 12 ? 5 : slot >= 6 ? 2 : 1, bw = slot - gap;
    int total = bars * slot - gap + (oct - 1) * group_gap;
    int x0 = (DISPLAY_USABLE_WIDTH - total) / 2;
    for (int b = 0; b < bars; b++) {
        int group = chord ? b / n : b;
        int h = hi > lo ? 2 + (pitch[b] - lo) * (STRIP_H - 2) / (hi - lo) : STRIP_H / 2;
        int bx = x0 + b * slot + (chord ? b / n * group_gap : 0), by = STRIP_Y + STRIP_H - h;
        display_fill_rect(1, bx, by, bw, h, true);
        if (group != cur && bw > 2 && h > 2) {
            display_fill_rect(1, bx + 1, by + 1, bw - 2, h - 2, false);
        }
    }
}

void arp_ui_tick(int64_t now_us, bool open)
{
    if (save_at == 1) {
        save_at = now_us + SAVE_DELAY_US;
    } else if (save_at && now_us >= save_at) {
        save_at = 0;
        save();
    }
    static int64_t next_at;
    if (!open) {
        strip_sig = ~0u;
        return;
    }
    if (now_us < next_at) {
        return;
    }
    uint8_t notes[CYCLE_MAX];
    bool chord = arp_cfg.mode == ARP_CHORD;
    int n = chord ? arp_held(notes, CYCLE_MAX) : arp_cycle(notes, CYCLE_MAX);
    uint32_t sig = (uint32_t)(arp_step() + 2) * 977u + n + (chord ? arp_cfg.octaves * 7919u : 0);
    for (int k = 0; k < n; k++) {
        sig = sig * 31u + notes[k];
    }
    if (sig != strip_sig) {
        strip_sig = sig;
        next_at = now_us + REDRAW_US;
        draw_strip();
    }
}

static void column(int col, const char *name, const char *value)
{
    int d = col / 4, x = (col % 4) * COLUMN_WIDTH;
    display_fill_rect(d, x, STRIP_Y + STRIP_H + 3, COLUMN_WIDTH, DISPLAY_HEIGHT - STRIP_Y - STRIP_H - 3, false);
    centered(d, x, COLUMN_INNER, NAME_CAP_Y, &font_small, name, true);
    const font_t *vf = display_text_width(&font_medium, value) <= COLUMN_INNER - 2 ? &font_medium : &font_small;
    centered(d, x, COLUMN_INNER, VALUE_CAP_Y, vf, value, true);
}

static void draw_head(void)
{
    char buf[48];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, 0, DISPLAY_WIDTH, HEADER_LINE_Y + 1, false);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    snprintf(buf, sizeof buf, "ARPEGGIATOR  %s", arp_cfg.on ? "ON" : "OFF");
    text_cap(0, 2, 1, &font_small, buf, true);
    snprintf(buf, sizeof buf, "%d BPM  SOLO: on/off", sq_pattern.bpm);
    text_cap(1, 2, 1, &font_small, buf, true);
}

void arp_ui_draw(void)
{
    char b[16];
    draw_head();
    strip_sig = ~0u;  /* the picture follows in the next tick */
    column(0, "MODE", arp_mode_names[arp_cfg.mode]);
    column(1, "RATE", arp_rate_names[arp_cfg.rate]);
    snprintf(b, sizeof b, "%d%%", arp_cfg.gate);
    column(2, "GATE", b);
    snprintf(b, sizeof b, "%d", arp_cfg.octaves);
    column(3, "OCTAVES", b);
    snprintf(b, sizeof b, "%d%%", arp_cfg.swing);
    column(4, "SWING", b);
    if (arp_cfg.vel) {
        snprintf(b, sizeof b, "%d", arp_cfg.vel);
    } else {
        snprintf(b, sizeof b, "HELD");
    }
    column(5, "VELOCITY", b);
    column(6, "LATCH", arp_cfg.latch ? "ON" : "OFF");
    column(7, "RETRIG", arp_cfg.retrig ? "ON" : "OFF");
}

void arp_ui_toggle(void)
{
    arp_cfg.on = !arp_cfg.on;
    arp_changed();
    touched();
}

void arp_ui_encoder(int i, int delta)
{
    static const int units[8] = {60, 60, 20, 80, 12, 8, 60, 60};
    acc[i] += delta;
    int n = acc[i] / units[i];
    if (!n) {
        return;
    }
    acc[i] -= n * units[i];
    switch (i) {
    case 0:
        arp_cfg.mode = clampi(arp_cfg.mode + n, 0, ARP_MODES - 1);
        break;
    case 1:
        arp_cfg.rate = clampi(arp_cfg.rate + n, 0, ARP_RATES - 1);
        break;
    case 2:
        arp_cfg.gate = clampi(arp_cfg.gate + n * 5, 5, 100);
        break;
    case 3:
        arp_cfg.octaves = clampi(arp_cfg.octaves + n, 1, 4);
        break;
    case 4:
        arp_cfg.swing = clampi(arp_cfg.swing + n, 50, 75);
        break;
    case 5:
        arp_cfg.vel = clampi(arp_cfg.vel + n, 0, 127);
        break;
    case 6:
        arp_cfg.latch = n > 0;
        break;
    default:
        arp_cfg.retrig = n > 0;
        break;
    }
    arp_changed();
    touched();
    arp_ui_draw();
}

void arp_ui_main_encoder(int steps)
{
    sq_set_bpm(sq_pattern.bpm + steps);
    draw_head();
}

static const synth_tip_t tips[] = {
    {"ARPEGGIATOR", {"SOLO switches it on and off, on every", "screen (lit = on). SHIFT + SOLO: these", "settings.",
                     "The held pads play one after another", "on the pad channel, on the grid of", "RATE: in time with the sequencer while",
                     "it plays."},
     {BTN_SOLO, BTN_SHIFT, BTN_PLAY, -1, -1, -1, -1, -1}},
    {"PATTERN", {"MODE: UP, DOWN, UP-DOWN, DOWN-UP, ORDER", "(the order you pressed), RANDOM, CHORD", "(all held notes at once).",
                 "OCTAVES repeats it higher. RATE: 1/4 -", "1/32, T = triplets. SWING delays every", "second note. The picture above the knobs",
                 "shows one pass, the playing note filled."},
     {-1, -1, -1, -1, -1, -1, -1, -1}},
    {"PLAYING", {"GATE: how long a note lasts. VELOCITY:", "HELD = the pad's own, else a fixed one.", "Knob 7 LATCH: the notes stay",
                 "after the pads are let go, until a new", "group is played. Knob 8 RETRIG: a new", "pad starts the pattern over.",
                 "Main encoder: tempo. MUTE: panic."},
     {BTN_MUTE, BTN_TEMPO, -1, -1, -1, -1, -1, -1}},
};

const synth_tip_t *arp_ui_tips(int *count)
{
    *count = sizeof tips / sizeof tips[0];
    return tips;
}
