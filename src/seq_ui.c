#include <stdio.h>
#include <string.h>
#include "buttons.h"
#include "display.h"
#include "esp_timer.h"
#include "maschine.h"
#include "seq_ui.h"
#include "sequencer.h"

#define COLUMN_WIDTH 64
#define COLUMN_INNER 63
#define HEADER_LINE_Y 11
#define TOP_Y 13            /* page blocks (left) and the page's steps (right) */
#define SWITCH_Y 27
#define SWITCH_H 11
#define NAME_CAP_Y 41
#define VALUE_CAP_Y 52
#define LED_DIM 9
#define LED_ON 127
#define REDRAW_US 50000     /* LEDs and displays: never faster (see CLAUDE.md) */
#define SETTINGS_FILE "/lfs/seq.cfg"

/* One colour per track, so the picked one is never in doubt */
static const uint8_t track_color[SQ_TRACKS][3] = {
    {127, 10, 10}, {127, 50, 0}, {110, 100, 0}, {10, 110, 10},
    {0, 100, 100}, {20, 40, 127}, {90, 10, 120}, {127, 20, 70},
};

static int sel_track, sel_step, page;
static bool follow = true;       /* the page follows the playing track */
static bool rec_write = true;    /* Rec: the pads switch steps on and off; off: they only pick the step */
static bool settings_open;       /* Control: the sequencer's settings */
static int in_channel;           /* MIDI IN channel that fills steps, 0 = off, else 1-16 */
static int acc[8];
static uint8_t last_note = 60, last_vel = 100, last_gate = 50;
static bool dirty_head, dirty_top, dirty_cols, dirty_leds;
static int64_t next_at;
static int shown_pos = -2;
static bool shown_playing;

/* Step entry from MIDI IN: the key's duration becomes the gate or a chain of ties */
static int keys_down;
static int64_t keys_t0;
static int entry_note, entry_vel;

static sq_track_t *track(void) { return &sq_pattern.track[sel_track]; }
static int pages_of(const sq_track_t *t) { return (t->length + 15) / 16; }
static int pad_index(int pad) { return (3 - pad / 4) * 4 + pad % 4; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void note_name(int note, char *buf, size_t len)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[note % 12], note / 12 - 1);
}

static int text_cap(int d, int x, int cap_y, const font_t *f, const char *s, bool on)
{
    return display_text(d, x, cap_y - f->cap_top, f, s, on);
}

static void centered(int d, int x, int w, int cap_y, const font_t *f, const char *s, bool on)
{
    text_cap(d, x + (w - display_text_width(f, s)) / 2, cap_y, f, s, on);
}

static void clamp_state(void)
{
    sq_track_t *t = track();
    if (sel_step >= t->length) {
        sel_step = t->length - 1;
    }
    if (page >= pages_of(t)) {
        page = pages_of(t) - 1;
    }
}

static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (f) {
        int c = fgetc(f);
        fclose(f);
        in_channel = c >= 0 && c <= 16 ? c : 0;
    }
}

static void save_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (f) {
        fputc(in_channel, f);
        fclose(f);
    }
}

static void draw_head(void)
{
    const sq_track_t *t = track();
    char buf[48];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, 0, DISPLAY_WIDTH, HEADER_LINE_Y + 1, false);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    if (settings_open) {
        text_cap(0, 2, 1, &font_small, "SEQUENCER SETTINGS", true);
        text_cap(1, 2, 1, &font_small, "Control: back", true);
        return;
    }
    snprintf(buf, sizeof buf, "SEQUENCER  TRACK %d  CH %d", sel_track + 1, t->channel + 1);
    text_cap(0, 2, 1, &font_small, buf, true);
    snprintf(buf, sizeof buf, "%d BPM  %s  %s", sq_pattern.bpm, sq_playing() ? "PLAYING" : "STOPPED",
             rec_write ? "WRITE" : "SELECT");
    text_cap(1, 2, 1, &font_small, buf, true);
}

static void draw_top(void)
{
    const sq_track_t *t = track();
    int pos = sq_position(sel_track);
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, HEADER_LINE_Y + 1, DISPLAY_WIDTH, 14, false);
    }
    /* Left: the pages (16 steps each), the shown one inverted, the playing one marked */
    int pages = pages_of(t);
    for (int i = 0; i < 8; i++) {
        int x = i * 32 + 1;
        if (i >= pages) {
            display_fill_rect(0, x + 10, TOP_Y + 5, 8, 1, true);
            continue;
        }
        bool shown = i == page;
        if (shown) {
            display_fill_rect(0, x, TOP_Y, 29, 10, true);
        } else {
            display_rect(0, x, TOP_Y, 29, 10, true);
        }
        char n[4];
        snprintf(n, sizeof n, "%d", i + 1);
        centered(0, x, 29, TOP_Y + 1, &font_small, n, !shown);
        if (pos >= 0 && pos / 16 == i) {
            display_fill_rect(0, x + 7, TOP_Y + 12, 15, 2, true);
        }
    }
    /* Right: the shown page's steps. On = solid, tied = hatched and joined to the one before, a ratchet
     * cuts the cell into its hits, the picked step has a frame, the playing one a bar below */
    for (int s = 0; s < 16; s++) {
        int idx = page * 16 + s, x = s * 16;
        if (idx >= t->length) {
            display_fill_rect(1, x + 6, TOP_Y + 4, 3, 1, true);
            continue;
        }
        const sq_step_t *st = &t->steps[idx];
        if (!st->vel) {
            display_rect(1, x, TOP_Y, 14, 9, true);
        } else if (st->tie) {
            display_rect(1, x, TOP_Y, 14, 9, true);
            display_fill_rect(1, x, TOP_Y + 3, 14, 3, true);
            display_fill_rect(1, x - 2, TOP_Y + 3, 2, 3, true);
        } else {
            display_fill_rect(1, x, TOP_Y, 14, 9, true);
            for (int k = 1; k < st->ratchet && k < 8; k++) {
                display_fill_rect(1, x + 14 * k / st->ratchet, TOP_Y + 1, 1, 7, false);
            }
        }
        if (idx == sel_step) {
            display_rect(1, x - 1, TOP_Y - 1, 16, 11, true);
        }
        if (idx == pos) {
            display_fill_rect(1, x + 3, TOP_Y + 12, 8, 2, true);
        }
    }
}

static void column(int col, const char *name, const char *value)
{
    int d = col / 4, x = (col % 4) * COLUMN_WIDTH;
    display_fill_rect(d, x, SWITCH_Y - 1, COLUMN_WIDTH, DISPLAY_HEIGHT - SWITCH_Y + 1, false);
    centered(d, x, COLUMN_INNER, NAME_CAP_Y, &font_small, name, true);
    centered(d, x, COLUMN_INNER, VALUE_CAP_Y, &font_medium, value, true);
}

/* The switch on a column's display button, above its knob */
static void switch_box(int col, const char *name, bool on)
{
    int d = col / 4, x = (col % 4) * COLUMN_WIDTH;
    display_round_rect(d, x + 1, SWITCH_Y, COLUMN_INNER - 2, SWITCH_H, on, true);
    centered(d, x, COLUMN_INNER, SWITCH_Y + (SWITCH_H - font_small.cap_height) / 2, &font_small, name, !on);
}

static void draw_cols(void)
{
    const sq_track_t *t = track();
    const sq_step_t *s = &t->steps[sel_step];
    char b[16];
    if (s->vel) {
        note_name(s->note[0], b, sizeof b);
    } else {
        snprintf(b, sizeof b, "--");
    }
    column(0, "NOTE", b);
    switch_box(0, "TIE", s->vel && s->tie);
    snprintf(b, sizeof b, "%d", s->vel);
    column(1, "VELOCITY", b);
    snprintf(b, sizeof b, "%d%%", s->gate);
    column(2, "GATE", b);
    snprintf(b, sizeof b, "%dx", s->ratchet);
    column(3, "RATCHET", b);
    snprintf(b, sizeof b, "%d", t->channel + 1);
    column(4, "CHANNEL", b);
    switch_box(4, "MUTE", t->mute);
    snprintf(b, sizeof b, "%d", t->length);
    column(5, "LENGTH", b);
    snprintf(b, sizeof b, t->transpose > 0 ? "+%d" : "%d", t->transpose);
    column(6, "TRANSPOSE", b);
    snprintf(b, sizeof b, "%d", sq_pattern.bpm);
    column(7, "TEMPO", b);
}

static void draw_settings(void)
{
    char b[16];
    if (in_channel) {
        snprintf(b, sizeof b, "CH %d", in_channel);
    } else {
        snprintf(b, sizeof b, "OFF");
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, HEADER_LINE_Y + 1, DISPLAY_WIDTH, DISPLAY_HEIGHT - HEADER_LINE_Y - 1, false);
    }
    column(0, "IN CHANNEL", b);
    text_cap(1, 2, 20, &font_small, "A note on the IN channel is", true);
    text_cap(1, 2, 32, &font_small, "written to the picked step:", true);
    text_cap(1, 2, 44, &font_small, "pitch, velocity, length.", true);
}

/* The picked step's pad pulses, so it stands out among the rest */
static int pulse_pct(void)
{
    int ph = (int)((esp_timer_get_time() / 1000) % 900);
    int v = ph < 450 ? ph : 900 - ph;
    return 30 + 70 * v / 450;
}

static void pad_leds(void)
{
    const sq_track_t *t = track();
    int pos = sq_position(sel_track);
    const uint8_t *c = track_color[sel_track];
    int pc = pulse_pct();
    for (int pad = 0; pad < 16; pad++) {
        int idx = page * 16 + pad_index(pad);
        uint8_t r, g, b;
        if (idx >= t->length) {
            r = g = b = 0;
        } else if (idx == pos) {
            r = g = b = 127;
        } else if (t->steps[idx].vel) {
            r = c[0], g = c[1], b = c[2];
        } else {
            r = 4, g = 4, b = 5;
        }
        if (idx == sel_step && idx < t->length) {
            if (r + g + b < 30) {
                r = g = 40, b = 50;  /* an empty step is dark: give the pulse something to dim */
            }
            r = r * pc / 100, g = g * pc / 100, b = b * pc / 100;
        }
        maschine_set_pad_led(pad, r, g, b);
    }
}

void seq_ui_draw(void)
{
    clamp_state();
    draw_head();
    if (settings_open) {
        draw_settings();
    } else {
        draw_top();
        draw_cols();
    }
    pad_leds();
    shown_pos = sq_position(sel_track);
    shown_playing = sq_playing();
    dirty_head = dirty_top = dirty_cols = dirty_leds = false;
}

void seq_ui_enter(void)
{
    static bool loaded;
    if (!loaded) {
        loaded = true;
        load_settings();
    }
    follow = true;
    settings_open = false;
    keys_down = 0;
    memset(acc, 0, sizeof acc);
    seq_ui_draw();
}

void seq_ui_leave(void)
{
    if (settings_open) {
        save_settings();
    }
    settings_open = false;
}

void seq_ui_tick(int64_t now_us)
{
    int pos = sq_position(sel_track);
    if (pos != shown_pos) {
        if (follow && pos >= 0 && pos / 16 != page) {
            page = pos / 16;
            dirty_leds = dirty_cols = true;
        }
        dirty_top = dirty_leds = true;
    }
    if (sq_playing() != shown_playing) {
        dirty_head = true;
    }
    if (now_us >= next_at && sel_step / 16 == page) {
        dirty_leds = true;  /* the pulse */
    }
    if (now_us < next_at || !(dirty_head || dirty_top || dirty_cols || dirty_leds)) {
        return;
    }
    next_at = now_us + REDRAW_US;
    clamp_state();
    if (dirty_head) {
        draw_head();
    }
    if (!settings_open) {
        if (dirty_top) {
            draw_top();
        }
        if (dirty_cols) {
            draw_cols();
        }
    }
    if (dirty_leds || dirty_top) {
        pad_leds();
    }
    shown_pos = pos;
    shown_playing = sq_playing();
    dirty_head = dirty_top = dirty_cols = dirty_leds = false;
}

static void select_track(int tr)
{
    sel_track = tr;
    sel_step = 0;
    page = 0;
    follow = true;
    clamp_state();
    int pos = sq_position(sel_track);
    if (pos >= 0) {
        page = pos / 16;
    }
    dirty_head = dirty_top = dirty_cols = dirty_leds = true;
    shown_pos = -2;
}

static void toggle_settings(void)
{
    if (settings_open) {
        save_settings();
    }
    settings_open = !settings_open;
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    seq_ui_draw();
}

bool seq_ui_button(int bit, bool down)
{
    if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + SQ_TRACKS) {
        if (down) {
            select_track(bit - BTN_GROUP_A);
        }
        return true;
    }
    sq_track_t *t = track();
    sq_step_t *st = &t->steps[sel_step];
    switch (bit) {
    case BTN_CONTROL:
        if (down) {
            toggle_settings();
        }
        return true;
    case BTN_REC:
        if (down) {
            rec_write = !rec_write;
            dirty_head = true;
        }
        return true;
    case BTN_DISPLAY_1:
        if (down && !settings_open && st->vel) {
            st->tie = !st->tie;
            dirty_top = dirty_cols = true;
        }
        return true;
    case BTN_DISPLAY_1 + 4:
        if (down && !settings_open) {
            t->mute = !t->mute;
            dirty_cols = true;
        }
        return true;
    case BTN_RESTART:
        if (down) {
            sq_restart();
            follow = true;
        }
        return true;
    case BTN_STEP_LEFT:
    case BTN_STEP_RIGHT:
        if (down && !settings_open) {
            int p = page + (bit == BTN_STEP_LEFT ? -1 : 1);
            if (p >= 0 && p < pages_of(t)) {
                page = p;
                follow = false;
                dirty_top = dirty_leds = true;
            }
        }
        return true;
    case BTN_ERASE:
        if (down && !settings_open) {
            st->vel = 0;
            st->tie = false;
            dirty_top = dirty_cols = dirty_leds = true;
        }
        return true;
    default:
        return false;
    }
}

/* A step being switched on takes the note of the nearest step before it (else the last note used) */
static void new_step(sq_step_t *s)
{
    const sq_track_t *t = track();
    int idx = (int)(s - t->steps), note = last_note;
    for (int i = idx - 1; i >= 0; i--) {
        if (t->steps[i].vel) {
            note = t->steps[i].note[0];
            break;
        }
    }
    s->note[0] = note;
    s->vel = last_vel;
    s->gate = last_gate;
    s->ratchet = 1;
    s->tie = false;
}

void seq_ui_pad(int pad, bool down, int raw)
{
    if (!down || settings_open) {
        return;
    }
    sq_track_t *t = track();
    int idx = page * 16 + pad_index(pad);
    if (idx >= t->length) {
        return;
    }
    sel_step = idx;
    if (rec_write) {
        sq_step_t *s = &t->steps[idx];
        if (s->vel) {
            s->vel = 0;
            s->tie = false;
        } else {
            new_step(s);
        }
    }
    dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_encoder(int i, int delta)
{
    static const int units[8] = {30, 8, 12, 60, 60, 20, 40, 25};
    if (settings_open) {
        if (i == 0) {
            acc[0] += delta;
            int n = acc[0] / 60;
            if (n) {
                acc[0] -= n * 60;
                in_channel = clampi(in_channel + n, 0, 16);
                draw_settings();
            }
        }
        return;
    }
    acc[i] += delta;
    int n = acc[i] / units[i];
    if (!n) {
        return;
    }
    acc[i] -= n * units[i];
    sq_track_t *t = track();
    sq_step_t *s = &t->steps[sel_step];
    switch (i) {
    case 0:
        s->note[0] = last_note = clampi(s->note[0] + n, 0, 127);
        break;
    case 1:
        if (!s->vel && n > 0) {
            new_step(s);
        } else {
            s->vel = clampi(s->vel + n, 0, 127);
            if (s->vel) {
                last_vel = s->vel;
            }
        }
        break;
    case 2:
        s->gate = last_gate = clampi(s->gate + n * 5, 5, 100);
        break;
    case 3:
        s->ratchet = clampi(s->ratchet + n, 1, 8);
        break;
    case 4:
        t->channel = clampi(t->channel + n, 0, 15);
        break;
    case 5:
        t->length = clampi(t->length + n, 1, SQ_STEPS);
        break;
    case 6:
        t->transpose = clampi(t->transpose + n, -24, 24);
        break;
    default:
        sq_set_bpm(sq_pattern.bpm + n);
        break;
    }
    clamp_state();
    dirty_head = dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_main_encoder(int steps)
{
    sq_set_bpm(sq_pattern.bpm + steps);
    dirty_head = dirty_cols = true;
}

/* A key on the IN channel: the first one writes pitch and velocity into the picked step; when the last is
 * released the time it was held is the gate, or - over a step - a chain of ties; the pick moves on */
void seq_ui_midi_note(int channel, int note, int velocity)
{
    if (settings_open || sq_playing() || !in_channel || channel != in_channel - 1) {
        return;
    }
    sq_track_t *t = track();
    if (velocity) {
        if (!keys_down) {
            keys_t0 = esp_timer_get_time();
        }
        keys_down++;
        entry_note = note;
        entry_vel = velocity;
        sq_step_t *s = &t->steps[sel_step];
        new_step(s);
        s->note[0] = last_note = note;
        s->vel = last_vel = velocity;
        dirty_top = dirty_cols = dirty_leds = true;
        return;
    }
    if (keys_down == 0 || --keys_down > 0) {
        return;
    }
    int us_per_tick = 60000000 / (sq_pattern.bpm * SQ_PPQ);
    int ticks = (int)((esp_timer_get_time() - keys_t0) / us_per_tick);
    int steps = (ticks + SQ_STEP_TICKS - 1) / SQ_STEP_TICKS;
    steps = clampi(steps, 1, t->length - sel_step);
    int last_ticks = ticks - (steps - 1) * SQ_STEP_TICKS;
    for (int k = 1; k < steps; k++) {
        sq_step_t *s = &t->steps[sel_step + k];
        new_step(s);
        s->note[0] = entry_note;
        s->vel = entry_vel;
        s->tie = true;
    }
    sq_step_t *end = &t->steps[sel_step + steps - 1];
    end->gate = last_gate = clampi(last_ticks * 100 / SQ_STEP_TICKS, 5, 100);
    sel_step = clampi(sel_step + steps, 0, t->length - 1);
    page = sel_step / 16;
    follow = false;
    dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_group_leds(void)
{
    for (int g = 0; g < SQ_TRACKS; g++) {
        const uint8_t *c = track_color[g];
        int div = g == sel_track ? 1 : 6;
        maschine_set_group_led(g, c[0] / div, c[1] / div, c[2] / div);
    }
}

void seq_ui_button_leds(void)
{
    const sq_track_t *t = track();
    const sq_step_t *s = &t->steps[sel_step];
    for (int bit = 0; bit < 48; bit++) {
        if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8) {
            continue;
        }
        uint8_t v = 0;
        switch (bit) {
        case BTN_SCENE:
            v = LED_ON;
            break;
        case BTN_PLAY:
            v = sq_playing() ? LED_ON : LED_DIM;
            break;
        case BTN_REC:
            v = rec_write ? LED_ON : LED_DIM;
            break;
        case BTN_CONTROL:
            v = settings_open ? LED_ON : LED_DIM;
            break;
        case BTN_DISPLAY_1:
            v = settings_open || !s->vel ? 0 : s->tie ? LED_ON : LED_DIM;
            break;
        case BTN_DISPLAY_1 + 4:
            v = settings_open ? 0 : t->mute ? LED_ON : LED_DIM;
            break;
        case BTN_RESTART:
        case BTN_STEP_LEFT:
        case BTN_STEP_RIGHT:
        case BTN_ERASE:
        case BTN_STEP:
        case BTN_PAD_MODE:
        case BTN_MUTE:
        case BTN_TEMPO:  /* the main encoder sets the tempo here */
            v = LED_DIM;
            break;
        }
        maschine_set_button_led(bit, v);
    }
}
