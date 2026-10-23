#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "arp.h"
#include "buttons.h"
#include "display.h"
#include "esp_timer.h"
#include "maschine.h"
#include "midi.h"
#include "seq_ui.h"
#include "sequencer.h"
#include "synth.h"

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
#define SEQ_DIR "/lfs/seq"
#define SETTINGS_FILE SEQ_DIR "/settings"   /* IN channel, the pattern slot in use */
#define SLOTS 16
#define PATTERN_MAGIC 0x31505153u           /* "SQP1" */
#define SAVE_DELAY_US 2000000               /* a pattern is written this long after its last edit */

/* One colour per track, so the picked one is never in doubt */
static const uint8_t track_color[SQ_TRACKS][3] = {
    {127, 10, 10}, {127, 50, 0}, {110, 100, 0}, {10, 110, 10},
    {0, 100, 100}, {20, 40, 127}, {90, 10, 120}, {127, 20, 70},
};

static int sel_track, sel_step, page;
static bool follow = true;       /* the page follows the playing track */
static bool shift_down;          /* Shift + Rec: live recording */
static bool live;                /* the pads play notes (on the track's channel); while the sequencer runs they record */
static struct {
    bool held;
    uint8_t note;                /* what the pad sent */
    uint8_t vel;
    int idx;                     /* the step it was recorded into, -1 = not recorded */
    uint32_t t0;
} live_pad[16];
static int64_t clear_notice_until;  /* "PATTERN n CLEARED" on the patterns screen */
static int clear_notice_slot;
#define CLEAR_NOTICE_US 1500000
static int erase_last = -1;      /* live + Erase held: the last step cleared */
static bool rec_write = true;    /* Rec: the pads switch steps on and off; off: they only pick the step */
static bool settings_open;       /* Control: the sequencer's settings */
static bool pattern_open;        /* Pattern: the pads are the pattern slots */
static int cur_slot, queued_slot = -1;
static bool slot_used[SLOTS];
static bool pattern_dirty;
static bool erase_held, erase_used;  /* Erase + Group clears a track, Erase + pad (patterns) a slot */
static int64_t save_at;
static sq_pattern_t loaded;      /* scratch for reading a slot */
static int in_channel;           /* MIDI IN channel that fills steps, 0 = off, else 1-16 */
static int acc[8];
static uint8_t last_vel = 100, last_gate = 50;
static bool dirty_head, dirty_top, dirty_cols, dirty_leds;
static int64_t next_at;
static int shown_pos = -2;
static bool shown_playing;

/* Step entry from MIDI IN: the key's duration becomes the gate or a chain of ties */
static int keys_down;
static int64_t keys_t0;
static int entry_note, entry_vel;

static void live_off(void);
static void live_pad_event(int pad, bool down, int raw);
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
        int c = fgetc(f), slot = fgetc(f), clk = fgetc(f);
        fclose(f);
        in_channel = c >= 0 && c <= 16 ? c : 0;
        sq_clock_out = clk == 1;
        cur_slot = slot >= 0 && slot < SLOTS ? slot : 0;
    }
}

static void save_settings(void)
{
    mkdir(SEQ_DIR, 0777);  /* fails harmlessly if it exists */
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (f) {
        fputc(in_channel, f);
        fputc(cur_slot, f);
        fputc(sq_clock_out, f);
        fclose(f);
    }
}

static void slot_path(int slot, char *buf, size_t len)
{
    snprintf(buf, len, SEQ_DIR "/p%02d", slot + 1);
}

/* A slot's file: magic, size of the pattern struct (a layout change makes old files unreadable, not wrong), data */
static bool read_slot(int slot, sq_pattern_t *out)
{
    char path[32];
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint32_t head[2];
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == PATTERN_MAGIC && head[1] == sizeof *out &&
              fread(out, sizeof *out, 1, f) == 1;
    fclose(f);
    return ok;
}

static void write_slot(int slot)
{
    char path[32];
    mkdir(SEQ_DIR, 0777);
    slot_path(slot, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t head[2] = {PATTERN_MAGIC, sizeof sq_pattern};
        fwrite(head, sizeof head, 1, f);
        fwrite(&sq_pattern, sizeof sq_pattern, 1, f);
        fclose(f);
        slot_used[slot] = true;
    }
    pattern_dirty = false;
    save_at = 0;
}

/* Every edit of the pattern: it is written to its slot a moment later */
static void touched(void)
{
    pattern_dirty = true;
    save_at = esp_timer_get_time() + SAVE_DELAY_US;
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
    if (pattern_open) {
        text_cap(0, 2, 1, &font_small, "SEQUENCER PATTERNS", true);
        text_cap(1, 2, 1, &font_small, "Pattern: back", true);
        return;
    }
    snprintf(buf, sizeof buf, "SEQUENCER  TRACK %d  CH %d", sel_track + 1, t->channel + 1);
    text_cap(0, 2, 1, &font_small, buf, true);
    snprintf(buf, sizeof buf, "%d BPM  %s  %s", sq_pattern.bpm, sq_playing() ? "PLAYING" : "STOPPED",
             live ? "LIVE REC" : rec_write ? "WRITE" : "SELECT");
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
    column(1, "CLOCK OUT", sq_clock_out ? "ON" : "OFF");
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

static void draw_patterns(void)
{
    char b[24];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, HEADER_LINE_Y + 1, DISPLAY_WIDTH, DISPLAY_HEIGHT - HEADER_LINE_Y - 1, false);
    }
    text_cap(0, 4, 22, &font_small, "PATTERN", true);
    snprintf(b, sizeof b, "%d", cur_slot + 1);
    text_cap(0, 4, 36, &font_large, b, true);
    if (queued_slot >= 0) {
        snprintf(b, sizeof b, "NEXT: %d", queued_slot + 1);
        text_cap(0, 100, 36, &font_medium, b, true);
    }
    if (clear_notice_until) {
        snprintf(b, sizeof b, "PATTERN %d", clear_notice_slot + 1);
        text_cap(1, 6, 16, &font_medium, b, true);
        text_cap(1, 6, 36, &font_medium, "CLEARED", true);
        return;
    }
    text_cap(1, 2, 20, &font_small, "Pads: pick a pattern", true);
    text_cap(1, 2, 32, &font_small, "Erase + pad: clear it", true);
    text_cap(1, 2, 44, &font_small, "Playing: switch at the end", true);
}

static void pattern_pad_leds(void)
{
    bool blink = (esp_timer_get_time() / 150000) & 1;
    int pc = pulse_pct();
    for (int pad = 0; pad < 16; pad++) {
        int slot = pad_index(pad);
        uint8_t r = 3, g = 3, b = 3;
        if (slot_used[slot]) {
            r = 0, g = 30, b = 60;
        }
        /* The picked pattern pulses in its own colour (blue when it has something, grey when empty), never white */
        uint8_t br = slot_used[slot] ? 0 : 70, bg = slot_used[slot] ? 80 : 70, bb = slot_used[slot] ? 127 : 70;
        if (clear_notice_until && slot == clear_notice_slot) {
            r = 127, g = 0, b = 0;  /* just cleared */
        } else if (slot == queued_slot) {
            r = blink ? br : 0, g = blink ? bg : 0, b = blink ? bb : 0;
        } else if (slot == cur_slot) {
            r = br * pc / 100, g = bg * pc / 100, b = bb * pc / 100;
        }
        maschine_set_pad_led(pad, r, g, b);
    }
}

static void pad_leds(void)
{
    if (pattern_open) {
        pattern_pad_leds();
        return;
    }
    const sq_track_t *t = track();
    int pos = sq_position(sel_track);
    const uint8_t *c = track_color[sel_track];
    int pc = pulse_pct();
    if (live) {
        /* Notes: the track's colour, the scale's root brighter, a held pad white */
        for (int pad = 0; pad < 16; pad++) {
            bool root = synth_pad_note(pad) % 12 == synth_pad_root() % 12;
            int div = root ? 2 : 8;
            if (live_pad[pad].held) {
                maschine_set_pad_led(pad, 127, 127, 127);
            } else {
                maschine_set_pad_led(pad, c[0] / div, c[1] / div, c[2] / div);
            }
        }
        return;
    }
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
    } else if (pattern_open) {
        draw_patterns();
    } else {
        draw_top();
        draw_cols();
    }
    pad_leds();
    shown_pos = sq_position(sel_track);
    shown_playing = sq_playing();
    dirty_head = dirty_top = dirty_cols = dirty_leds = false;
}

/* At boot: the settings, which slots exist, and the pattern of the slot in use */
void seq_ui_init(void)
{
    load_settings();
    for (int i = 0; i < SLOTS; i++) {
        char path[32];
        struct stat st;
        slot_path(i, path, sizeof path);
        slot_used[i] = stat(path, &st) == 0;
    }
    if (read_slot(cur_slot, &loaded)) {
        sq_load(&loaded);
    }
}

void seq_ui_enter(void)
{
    follow = true;
    settings_open = pattern_open = false;
    erase_held = false;
    keys_down = 0;
    memset(acc, 0, sizeof acc);
    seq_ui_draw();
}

void seq_ui_leave(void)
{
    live_off();
    if (settings_open) {
        save_settings();
    }
    settings_open = pattern_open = false;
    if (pattern_dirty && !sq_pending()) {
        write_slot(cur_slot);
    }
}

void seq_ui_tick(int64_t now_us)
{
    if (queued_slot >= 0 && !sq_pending()) {
        /* The queued pattern has taken over (edits made while it waited went to the old one and are not kept) */
        cur_slot = queued_slot;
        queued_slot = -1;
        pattern_dirty = false;
        save_at = 0;
        sel_step = 0;
        page = 0;
        save_settings();
        dirty_head = dirty_top = dirty_cols = dirty_leds = true;
    }
    if (clear_notice_until && now_us >= clear_notice_until) {
        clear_notice_until = 0;
        dirty_head = dirty_top = dirty_cols = dirty_leds = true;
    }
    if (live && erase_held && sq_playing() && !settings_open && !pattern_open) {
        /* Erase held while recording: every step the playhead passes is cleared */
        sq_track_t *t = track();
        int pos = sq_position(sel_track);
        if (pos >= 0 && pos < t->length && pos != erase_last) {
            int i = erase_last >= 0 && erase_last < t->length ? (erase_last + 1) % t->length : pos;
            for (;; i = (i + 1) % t->length) {
                t->steps[i].vel = 0;
                t->steps[i].tie = false;
                if (i == pos) {
                    break;
                }
            }
            erase_last = pos;
            erase_used = true;
            touched();
            dirty_top = dirty_cols = dirty_leds = true;
        }
    } else {
        erase_last = -1;
    }
    if (save_at && now_us >= save_at && !sq_pending()) {
        write_slot(cur_slot);
    }
    if (pattern_open && now_us >= next_at) {
        dirty_leds = true;  /* the pulse and the blink */
    }
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
    if (pattern_open) {
        if (dirty_head || dirty_top || dirty_cols) {
            draw_patterns();
        }
    } else if (!settings_open) {
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

/* Erase + Group: the track's steps back to empty (channel, length, transpose and mute stay) */
static void clear_track(int tr)
{
    sq_pattern_clear(&loaded);
    memcpy(sq_pattern.track[tr].steps, loaded.track[tr].steps, sizeof sq_pattern.track[tr].steps);
    touched();
    dirty_top = dirty_cols = dirty_leds = true;
}

/* Erase + pad in the patterns screen: the slot is emptied (its file removed); the one in use is cleared in place,
 * keeping the tempo */
static void clear_slot(int slot)
{
    char path[32];
    slot_path(slot, path, sizeof path);
    remove(path);
    slot_used[slot] = false;
    sq_pattern_clear(&loaded);
    if (slot == cur_slot) {
        memcpy(sq_pattern.track, loaded.track, sizeof sq_pattern.track);
        pattern_dirty = false;
        save_at = 0;
        sel_step = 0;
        page = 0;
    } else if (slot == queued_slot) {
        loaded.bpm = sq_pattern.bpm;
        sq_load(&loaded);  /* the waiting copy is replaced by the empty one */
    }
    clear_notice_slot = slot;
    clear_notice_until = esp_timer_get_time() + CLEAR_NOTICE_US;
    dirty_head = dirty_top = dirty_cols = dirty_leds = true;
}

static void open_screen(bool settings, bool patterns)
{
    if (settings_open) {
        save_settings();
    }
    settings_open = settings;
    pattern_open = patterns;
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    seq_ui_draw();
}

/* A pattern slot picked on its pad: the one in use is written first; a stopped sequencer takes the new one at
 * once, a playing one when the longest track comes round */
static void pick_slot(int slot)
{
    if (slot == cur_slot && queued_slot < 0) {
        return;
    }
    if (pattern_dirty && !sq_pending()) {
        write_slot(cur_slot);
    }
    pattern_dirty = false;
    save_at = 0;
    if (!read_slot(slot, &loaded)) {
        sq_pattern_clear(&loaded);
        loaded.bpm = sq_pattern.bpm;  /* a new pattern keeps the tempo */
    }
    sq_load(&loaded);
    if (sq_playing()) {
        queued_slot = slot;
    } else {
        cur_slot = slot;
        queued_slot = -1;
        sel_step = 0;
        page = 0;
        save_settings();
    }
    dirty_head = dirty_top = dirty_cols = dirty_leds = true;
}

bool seq_ui_button(int bit, bool down)
{
    if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + SQ_TRACKS) {
        if (down && erase_held && !settings_open && !pattern_open) {
            erase_used = true;
            clear_track(bit - BTN_GROUP_A);
        } else if (down) {
            select_track(bit - BTN_GROUP_A);
        }
        return true;
    }
    sq_track_t *t = track();
    sq_step_t *st = &t->steps[sel_step];
    switch (bit) {
    case BTN_CONTROL:
        if (down) {
            open_screen(!settings_open, false);
        }
        return true;
    case BTN_PATTERN:
        if (down) {
            open_screen(false, !pattern_open);
        }
        return true;
    case BTN_REC:
        if (down && shift_down && !settings_open && !pattern_open) {
            live_off();
            live = true;
            erase_last = -1;
            dirty_head = dirty_leds = true;
        } else if (down && live) {
            live_off();
            dirty_head = dirty_leds = true;
        } else if (down) {
            rec_write = !rec_write;
            dirty_head = true;
        }
        return true;
    case BTN_DISPLAY_1:
        if (down && !settings_open && !pattern_open && st->vel) {
            st->tie = !st->tie;
            touched();
            dirty_top = dirty_cols = true;
        }
        return true;
    case BTN_DISPLAY_1 + 4:
        if (down && !settings_open && !pattern_open) {
            t->mute = !t->mute;
            touched();
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
        if (down && !settings_open && !pattern_open) {
            int p = page + (bit == BTN_STEP_LEFT ? -1 : 1);
            if (p >= 0 && p < pages_of(t)) {
                page = p;
                follow = false;
                dirty_top = dirty_leds = true;
            }
        }
        return true;
    case BTN_ERASE:
        /* Alone (let go without a Group or pad): clears the picked step */
        if (down) {
            erase_held = true;
            erase_used = false;
        } else if (erase_held) {
            erase_held = false;
            if (!erase_used && !settings_open && !pattern_open) {
                st->vel = 0;
                st->tie = false;
                touched();
                dirty_top = dirty_cols = dirty_leds = true;
            }
        }
        return true;
    default:
        return false;
    }
}

/* ---- live recording ---- */

static int clamp_note(int n) { return clampi(n, 0, 127); }

/* The engine will play step idx at its next turn, but it was just recorded: the live note is the sound */
static void mark_skip(int idx)
{
    int len = track()->length;
    if (sq_playing() && sq_position(sel_track) >= 0 && idx == (sq_position(sel_track) + 1) % len) {
        sq_skip_next(sel_track, idx);
    }
}

/* Live recording off: held pads are let go (their notes end) */
static void live_off(void)
{
    if (!live) {
        return;
    }
    live = false;
    for (int p = 0; p < 16; p++) {
        if (live_pad[p].held) {
            midi_send(0x80 | track()->channel, live_pad[p].note, 0);
            live_pad[p].held = false;
        }
    }
}

/* A pad: its note sounds on the track's channel; while playing it is written into the nearest 16th. A note is
 * stored without the track's transpose, so that it plays back as it sounded. Let go: the time held is the gate,
 * or - over a step - a chain of ties */
static void live_pad_event(int pad, bool down, int raw)
{
    sq_track_t *t = track();
    int len = t->length;
    if (down) {
        int note = clamp_note(synth_pad_note(pad)), vel = clampi(synth_pad_velocity(raw), 1, 127);
        live_pad[pad].held = true;
        live_pad[pad].note = note;
        live_pad[pad].vel = vel;
        live_pad[pad].idx = -1;
        midi_send(0x90 | t->channel, note, vel);
        int pos = sq_position(sel_track);
        if (!sq_playing() || pos < 0) {
            return;
        }
        int idx = sq_step_phase() >= SQ_STEP_TICKS / 2 ? (pos + 1) % len : pos;
        sq_step_t *s = &t->steps[idx];
        memset(s, 0, sizeof *s);
        s->note[0] = clamp_note(note - t->transpose);
        s->vel = last_vel = vel;
        s->gate = last_gate;
        s->ratchet = 1;
        live_pad[pad].idx = idx;
        live_pad[pad].t0 = sq_tick_count();
        mark_skip(idx);
        sel_step = idx;
        touched();
        dirty_top = dirty_cols = dirty_leds = true;
        return;
    }
    if (!live_pad[pad].held) {
        return;
    }
    live_pad[pad].held = false;
    midi_send(0x80 | t->channel, live_pad[pad].note, 0);
    int idx = live_pad[pad].idx;
    if (idx >= 0 && idx < len) {
        int ticks = (int)(sq_tick_count() - live_pad[pad].t0);
        int steps = clampi((ticks + SQ_STEP_TICKS - 1) / SQ_STEP_TICKS, 1, len - idx);
        int last_ticks = ticks - (steps - 1) * SQ_STEP_TICKS;
        for (int k = 1; k < steps; k++) {
            sq_step_t *s = &t->steps[idx + k];
            memset(s, 0, sizeof *s);
            s->note[0] = t->steps[idx].note[0];
            s->vel = live_pad[pad].vel;
            s->gate = last_gate;
            s->ratchet = 1;
            s->tie = true;
            mark_skip(idx + k);
        }
        t->steps[idx + steps - 1].gate = last_gate = clampi(last_ticks * 100 / SQ_STEP_TICKS, 5, 100);
        touched();
        dirty_top = dirty_cols = dirty_leds = true;
    }
    dirty_leds = true;
}

/* A step being switched on takes the note of the nearest step before it (else the Pad Mode screen's lowest pad,
 * the root of the chosen key and scale) */
static void new_step(sq_step_t *s)
{
    const sq_track_t *t = track();
    int idx = (int)(s - t->steps), note = synth_pad_root();
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
    if (live && !settings_open && !pattern_open) {
        live_pad_event(pad, down, raw);
        return;
    }
    if (!down || settings_open) {
        return;
    }
    if (pattern_open) {
        if (erase_held) {
            erase_used = true;
            clear_slot(pad_index(pad));
        } else {
            pick_slot(pad_index(pad));
        }
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
        touched();
    }
    dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_encoder(int i, int delta)
{
    static const int units[8] = {30, 8, 12, 60, 60, 20, 40, 25};
    if (settings_open) {
        if (i == 1) {
            acc[1] += delta;
            if (abs(acc[1]) >= 60) {
                sq_clock_out = acc[1] > 0;
                acc[1] = 0;
                save_settings();
                draw_settings();
            }
        }
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
    if (pattern_open) {
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
    {
        /* Along the Pad Mode screen's scale (a note outside it, from MIDI IN, moves to the next one in it) */
        int note = synth_scale_note(s->note[0], n);
        s->note[0] = note < 0 ? s->note[0] : note;
        break;
    }
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
    touched();
    dirty_head = dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_main_encoder(int steps)
{
    sq_set_bpm(sq_pattern.bpm + steps);
    touched();
    dirty_head = dirty_cols = true;
}

/* A key on the IN channel: the first one writes pitch and velocity into the picked step; when the last is
 * released the time it was held is the gate, or - over a step - a chain of ties; the pick moves on */
void seq_ui_midi_note(int channel, int note, int velocity)
{
    if (settings_open || pattern_open || sq_playing() || !in_channel || channel != in_channel - 1) {
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
        s->note[0] = note;
        s->vel = last_vel = velocity;
        touched();
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
    touched();
    dirty_top = dirty_cols = dirty_leds = true;
}

void seq_ui_shift(bool down)
{
    shift_down = down;
}

void seq_ui_group_leds(void)
{
    for (int g = 0; g < SQ_TRACKS; g++) {
        const uint8_t *c = track_color[g];
        int div = g == sel_track ? 1 : 6;
        maschine_set_group_led(g, c[0] / div, c[1] / div, c[2] / div);
    }
}

void seq_ui_button_leds(uint8_t control_led)
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
            v = live ? LED_ON * pulse_pct() / 100 : rec_write ? LED_ON : LED_DIM;
            if (control_led) {
                v = v * control_led / LED_ON;  /* Shift + Rec: live recording */
            }
            break;
        case BTN_CONTROL:
            v = control_led ? control_led : settings_open ? LED_ON : LED_DIM;
            break;
        case BTN_PATTERN:
            v = pattern_open ? LED_ON : LED_DIM;
            break;
        case BTN_SOLO:
            v = (arp_cfg.on ? LED_ON : LED_DIM) * (control_led ? control_led : LED_ON) / LED_ON;  /* Shift + Solo: the arpeggiator's settings */
            break;
        case BTN_SHIFT:
            v = control_led ? LED_ON : LED_DIM;  /* Shift + Control: the tutorial, Shift + Solo */
            break;
        case BTN_DISPLAY_1:
            v = settings_open || pattern_open || !s->vel ? 0 : s->tie ? LED_ON : LED_DIM;
            break;
        case BTN_DISPLAY_1 + 4:
            v = settings_open || pattern_open ? 0 : t->mute ? LED_ON : LED_DIM;
            break;
        case BTN_ERASE:
            v = erase_held ? LED_ON : LED_DIM;
            break;
        case BTN_RESTART:
        case BTN_STEP_LEFT:
        case BTN_STEP_RIGHT:
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

static const synth_tip_t tips[] = {
    {"SEQUENCER", {"SCENE opens it; it keeps playing when", "you leave. PLAY works on every screen,", "SCENE pulses while it plays.",
                   "GROUP A-H: the track (own colours,", "the picked one bright).", "Main encoder: the tempo.",
                   "RESTART: back to the first step."},
     {BTN_SCENE, BTN_PLAY, BTN_RESTART, BTN_GROUP_A, BTN_GROUP_A + 7, BTN_TEMPO, -1, -1}},
    {"STEPS", {"The pads are the shown page's 16", "steps; STEP arrows turn the page.", "REC lit = WRITE: a pad switches its",
               "step on or off. REC dim = SELECT:", "a pad only picks the step to edit.",
               "ERASE clears the picked step.", "The picked step's pad pulses."},
     {BTN_REC, BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_ERASE, -1, -1, -1, -1}},
    {"STEP EDIT", {"Knobs edit the picked step: NOTE", "(along the Pad Mode scale), VELOCITY,", "GATE, RATCHET (1-8 hits a step).",
                   "Display button 1: TIE - the step", "continues the one before it, a", "different note is played legato.",
                   "A new step takes the previous note."},
     {BTN_DISPLAY_1, -1, -1, -1, -1, -1, -1, -1}},
    {"TRACK", {"Knobs: CHANNEL, LENGTH (1-128 steps,", "each track its own), TRANSPOSE, TEMPO.", "Display button 5: MUTE the track.",
               "ERASE + a GROUP button: clear that", "track's steps."},
     {BTN_DISPLAY_1 + 4, BTN_ERASE, BTN_GROUP_A, BTN_GROUP_A + 7, -1, -1, -1, -1}},
    {"PATTERNS", {"PATTERN: 16 slots on the pads, kept", "on the flash and saved by themselves.", "While playing, the new pattern starts",
                  "at the end of the longest track.", "ERASE + a pad: clear that slot."},
     {BTN_PATTERN, BTN_ERASE, -1, -1, -1, -1, -1, -1}},
    {"LIVE REC", {"SHIFT + REC: the pads play notes on", "the track's channel; REC leaves it.", "While playing, a pad is written into the",
                  "nearest 16th (the time held: gate or", "ties; a new note replaces the step).", "ERASE held clears the steps it passes.",
                  "Leaving Scene turns it off."},
     {BTN_SHIFT, BTN_REC, BTN_ERASE, BTN_PLAY, -1, -1, -1, -1}},
    {"MIDI IN", {"CONTROL (in Scene): the IN CHANNEL.", "A note on it fills the picked step:", "pitch, velocity, and how long you hold",
                 "it = the gate, or a chain of ties.", "Then the pick moves on. Only while", "the sequencer is stopped."},
     {BTN_CONTROL, -1, -1, -1, -1, -1, -1, -1}},
};

const synth_tip_t *seq_ui_tips(int *count)
{
    *count = (int)(sizeof tips / sizeof tips[0]);
    return tips;
}
