/*
 * Korg Wavestation SR, SEQ: a wave sequence = up to 255 steps (wave, level, duration, crossfade, semitone, fine
 * tune) with a start step, a loop (start, end, repeats, forward or back and forth) and a modulation source. Every
 * step plays a PCM wave; a patch's wave number 0-31 plays a wave sequence of that bank.
 *
 * Layout: A STEP (the step, bank, wave, level, duration, crossfade, semitone, fine; display buttons INSERT / DELETE),
 * B LOOP (start, loop start, loop end, repeats + back and forth, modulation source and amount). The step is picked
 * with the STEP knob, the transport Step arrows (Shift: 8 steps).
 *
 * Listening: a sequence that is in the SR (imported, or read with AUTO) is played by a test patch (wave A = the
 * sequence) in the edit buffer - the SR's version, not our edits. Not yet: sending edits (the SR builds a sequence
 * only by commands, written straight into a RAM bank's sequence - to be agreed on first), the Grid overview, the
 * SR's own storing. Working now: IMPORT of the banks'
 * sequences into the library (/lfs/wsseq/NNN name, NNN = bank * 32 + n + 1; the file layout is in ws_seq.h), AUTO
 * (reads the READ BANK / READ SEQ one back from the unit), editing, saving to the library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "ws_banks.h"
#include "ws_seq.h"

static const char *TAG = "wsseq";

#define NAME_CHARS 8
#define NAME_LEN 16
#define OFF_LEN 16
#define OFF_LSTART 17
#define OFF_LEND 18
#define OFF_REPEATS 19
#define OFF_BACKFORTH 20
#define OFF_START 21
#define OFF_MODSRC 22
#define OFF_MODAMT 23
#define WAVES 486

enum {
    P_STEP, P_BANK, P_WAVE, P_LEVEL, P_DUR, P_XFADE, P_SEMI, P_FINE, P_INSERT, P_DELETE,
    P_START, P_LSTART, P_LEND, P_REPEATS, P_BACKFORTH, P_MODSRC, P_MODAMT,
    P_COUNT
};

static const char *const source_names[13] = {"KBD LIN", "KBD CTR", "VEL LIN", "VEL EXP", "LFO 1", "LFO 2",
                                             "ENV 1", "AFTERTCH", "AT + MW", "MOD WHL", "MIDI 1", "MIDI 2",
                                             "MOD PED"};

#define _ SYNTH_NONE
#define NO_SW {_, _, _, _, _, _, _, _}
enum { PG_STEP, PG_LOOP };
static const synth_page_t pages[] = {
    [PG_STEP] = {0, "STEP", {P_STEP, P_BANK, P_WAVE, P_LEVEL, P_DUR, P_XFADE, P_SEMI, P_FINE},
                 {P_INSERT, P_DELETE, _, _, _, _, _, _}},
    [PG_LOOP] = {1, "LOOP", {P_START, P_LSTART, P_LEND, P_REPEATS, P_MODSRC, P_MODAMT, _, _},
                 {_, _, _, P_BACKFORTH, _, _, _, _}},
};
#undef _

/* ---- settings ---- */

#define SETTING_BANK 1
#define SETTING_NUM 2
#define SETTING_IMPORT 3
static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    /* The sequence AUTO reads: bank 0-11 (RAM 1-3, ROM 4-11, Card), number 0-31 */
    [SETTING_BANK] = {"READ BANK", SET_NUMBER, 0, 11, 0},
    [SETTING_NUM] = {"READ SEQ", SET_NUMBER, 0, 31, 0},
    /* What IMPORT reads from the SR into the library: its banks, 32 sequences each */
    [SETTING_IMPORT] = {"IMPORT", SET_NUMBER, 0, 3, 0},
};

static void setting_note(int setting, char *buf, size_t len)
{
    if (setting == SETTING_BANK) {
        int b = synth_setting(SETTING_BANK);
        snprintf(buf, len, "%s", ws_bank_names[b < 0 ? 0 : b > 11 ? 11 : b]);
    } else if (setting == SETTING_IMPORT) {
        static const char *const what[] = {"RAM 1-3", "ROM 4-11", "CARD", "READ BANK"};
        snprintf(buf, len, "%s", what[synth_setting(SETTING_IMPORT) & 3]);
    }
}

/* ---- state ---- */

static uint8_t seq[WSSEQ_MAX_LEN], init_seq[WSSEQ_MAX_LEN], as_read[WSSEQ_MAX_LEN];
static bool loaded, from_unit, as_read_ok;
static int cur;  /* the step shown, 0-based */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int channel(void) { return synth_setting(SETTING_CHANNEL) & 0x0F; }
static int length(void) { return seq[OFF_LEN]; }
static uint8_t *step_at(int i) { return seq + WSSEQ_HEADER + i * WSSEQ_STEP_LEN; }

/* A one step sequence: wave 1, full level, 24 steps long (Edisyn's insert defaults) */
static void make_init(uint8_t *s)
{
    memset(s, 0, WSSEQ_MAX_LEN);
    memset(s, ' ', NAME_LEN);
    memcpy(s, "Init", 4);
    s[OFF_LEN] = 1;
    s[OFF_LSTART] = s[OFF_LEND] = s[OFF_START] = 1;
    uint8_t *st = s + WSSEQ_HEADER;
    st[6] = 24 >> 8;
    st[7] = 24;
    st[8] = 99;
    st[5] = 20;
}

static void ensure(void)
{
    if (loaded) {
        return;
    }
    loaded = true;
    make_init(init_seq);
    memcpy(seq, init_seq, sizeof(seq));
}

static int wave_of(const uint8_t *st) { return st[0] << 8 | st[1]; }
static int xfade_of(const uint8_t *st) { return st[4] << 8 | st[5]; }
static int dur_of(const uint8_t *st) { return st[6] << 8 | st[7]; }

static void fix_cur(void) { cur = length() ? clampi(cur, 0, length() - 1) : 0; }

/* ---- parameters ---- */

static bool is_step_param(int p) { return p <= P_FINE; }

static int get(int p)
{
    const uint8_t *st = length() ? step_at(cur) : NULL;
    switch (p) {
    case P_STEP: return length() ? cur + 1 : 0;
    case P_BANK: return st ? st[9] & 1 : 0;
    case P_WAVE: return st ? wave_of(st) : 0;
    case P_LEVEL: return st ? st[8] : 0;
    case P_DUR: return st ? dur_of(st) : 1;
    case P_XFADE: return st ? xfade_of(st) : 0;
    case P_SEMI: return st ? (int8_t)st[2] : 0;
    case P_FINE: return st ? (int8_t)st[3] : 0;
    case P_START: return seq[OFF_START];
    case P_LSTART: return seq[OFF_LSTART];
    case P_LEND: return seq[OFF_LEND];
    case P_REPEATS: return seq[OFF_REPEATS];
    case P_BACKFORTH: return seq[OFF_BACKFORTH];
    case P_MODSRC: return seq[OFF_MODSRC];
    case P_MODAMT: return (int8_t)seq[OFF_MODAMT];
    default: return 0;
    }
}

static bool range_of(int p, int *lo, int *hi)
{
    int len = length();
    switch (p) {
    case P_STEP: *lo = len ? 1 : 0; *hi = len; return true;
    case P_BANK: *lo = 0; *hi = 1; return len > 0;
    case P_WAVE: *lo = 0; *hi = WAVES - 1; return len > 0;
    case P_LEVEL: *lo = 0; *hi = 99; return len > 0;
    case P_DUR: *lo = 1; *hi = 500; return len > 0;
    case P_XFADE: *lo = 0; *hi = 998; return len > 0;
    case P_SEMI: *lo = -24; *hi = 24; return len > 0;
    case P_FINE: *lo = -99; *hi = 99; return len > 0;
    case P_START: *lo = 1; *hi = len ? len : 1; return true;
    case P_LSTART: *lo = 1; *hi = seq[OFF_LEND] ? seq[OFF_LEND] : 1; return true;
    case P_LEND: *lo = seq[OFF_LSTART]; *hi = len ? len : 1; return true;
    case P_REPEATS: *lo = 0; *hi = 127; return true;
    case P_BACKFORTH: *lo = 0; *hi = 1; return true;
    case P_MODSRC: *lo = 0; *hi = 12; return true;
    case P_MODAMT: *lo = -127; *hi = 127; return true;
    default: return false;
    }
}

static void put(int p, int v)
{
    uint8_t *st = length() ? step_at(cur) : NULL;
    switch (p) {
    case P_STEP: cur = clampi(v - 1, 0, length() ? length() - 1 : 0); break;
    case P_BANK: st[9] = v ? 1 : 0; break;
    case P_WAVE: st[0] = v >> 8; st[1] = v; break;
    case P_LEVEL: st[8] = v; break;
    case P_DUR: st[6] = v >> 8; st[7] = v; break;
    case P_XFADE: st[4] = v >> 8; st[5] = v; break;
    case P_SEMI: st[2] = (uint8_t)v; break;
    case P_FINE: st[3] = (uint8_t)v; break;
    case P_START: seq[OFF_START] = v; break;
    case P_LSTART: seq[OFF_LSTART] = v; break;
    case P_LEND: seq[OFF_LEND] = v; break;
    case P_REPEATS: seq[OFF_REPEATS] = v; break;
    case P_BACKFORTH: seq[OFF_BACKFORTH] = v; break;
    case P_MODSRC: seq[OFF_MODSRC] = v; break;
    case P_MODAMT: seq[OFF_MODAMT] = (uint8_t)v; break;
    default: break;
    }
}

static int default_of(int p)
{
    switch (p) {
    case P_DUR: return 24;
    case P_XFADE: return 20;
    case P_LEVEL: return 99;
    case P_START: case P_LSTART: case P_LEND: return 1;
    default: return 0;
    }
}

/* ---- inserting and deleting steps ---- */

/* The loop and start numbers follow the steps: those after the one inserted / deleted move with it */
static void shift_marks(int at, int d)
{
    static const uint8_t offs[3] = {OFF_START, OFF_LSTART, OFF_LEND};
    for (int i = 0; i < 3; i++) {
        int v = seq[offs[i]];
        if (v > at) {
            v += d;
        }
        seq[offs[i]] = clampi(v, 1, length() ? length() : 1);
    }
    if (seq[OFF_LEND] < seq[OFF_LSTART]) {
        seq[OFF_LEND] = seq[OFF_LSTART];
    }
}

/* A copy of the step after it (an empty sequence gets the default step) */
static bool insert_step(void)
{
    int len = length();
    if (len >= WSSEQ_MAX_STEPS) {
        synth_notice("SEQUENCE FULL (255 STEPS)");
        return false;
    }
    if (len == 0) {
        memcpy(step_at(0), init_seq + WSSEQ_HEADER, WSSEQ_STEP_LEN);
        seq[OFF_LEN] = 1;
        cur = 0;
        return true;
    }
    memmove(step_at(cur + 2), step_at(cur + 1), (len - cur - 1) * WSSEQ_STEP_LEN);
    memcpy(step_at(cur + 1), step_at(cur), WSSEQ_STEP_LEN);
    seq[OFF_LEN] = len + 1;
    shift_marks(cur + 1, 1);
    cur++;
    return true;
}

static bool delete_step(void)
{
    int len = length();
    if (len == 0) {
        synth_notice("NO STEPS TO DELETE");
        return false;
    }
    memmove(step_at(cur), step_at(cur + 1), (len - cur - 1) * WSSEQ_STEP_LEN);
    seq[OFF_LEN] = len - 1;
    shift_marks(cur + 1, -1);
    fix_cur();
    return true;
}

/* ---- names ---- */

static const char *param_name(int p)
{
    static const char *const names[P_COUNT] = {
        [P_STEP] = "STEP", [P_BANK] = "BANK", [P_WAVE] = "WAVE", [P_LEVEL] = "LEVEL", [P_DUR] = "DURATION",
        [P_XFADE] = "CROSSFADE", [P_SEMI] = "SEMITONE", [P_FINE] = "FINE", [P_INSERT] = "INSERT",
        [P_DELETE] = "DELETE", [P_START] = "START", [P_LSTART] = "LOOP START", [P_LEND] = "LOOP END",
        [P_REPEATS] = "REPEATS", [P_BACKFORTH] = "BACK+FORTH", [P_MODSRC] = "MOD SOURCE", [P_MODAMT] = "MOD AMOUNT",
    };
    return p >= 0 && p < P_COUNT ? names[p] : "";
}

static void param_format(int p, char *buf, size_t len)
{
    ensure();
    if (is_step_param(p) && p != P_STEP && length() == 0) {
        snprintf(buf, len, "--");
        return;
    }
    int v = get(p);
    switch (p) {
    case P_STEP:
        snprintf(buf, len, "%d/%d", v, length());
        break;
    case P_BANK:
        snprintf(buf, len, v ? "CARD" : "ROM");
        break;
    case P_WAVE:
        snprintf(buf, len, "%d", v + 1);
        break;
    case P_DUR:
        snprintf(buf, len, v == 500 ? "GATE" : "%d", v);
        break;
    case P_SEMI:
    case P_FINE:
    case P_MODAMT:
        snprintf(buf, len, v ? "%+d" : "0", v);
        break;
    case P_REPEATS:
        snprintf(buf, len, v == 0 ? "OFF" : v == 127 ? "INF" : "%d", v);
        break;
    case P_BACKFORTH:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    case P_MODSRC:
        snprintf(buf, len, "%s", source_names[clampi(v, 0, 12)]);
        break;
    case P_INSERT:
    case P_DELETE:
        buf[0] = 0;
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    ensure();
    int lo, hi;
    if (p == P_BANK || p == P_INSERT || p == P_DELETE || p == P_BACKFORTH || p == P_MODSRC || !range_of(p, &lo, &hi) ||
        hi == lo) {
        return -1;
    }
    return (get(p) - lo) * 1000 / (hi - lo);
}

static bool param_on(int p)
{
    ensure();
    return p == P_BACKFORTH && get(p);
}

static int param_units(int p)
{
    switch (p) {
    case P_STEP: return 30;  /* a step a detent; the long sequences by turning fast (accelerated) */
    case P_WAVE: return 6;   /* 486 values: fast turns accelerate */
    case P_DUR: case P_XFADE: return 5;
    case P_BANK: case P_BACKFORTH: return 80;
    case P_MODSRC: return 25;
    default: return 10;
    }
}

static bool param_step(int p, int steps)
{
    ensure();
    int lo, hi;
    if (!range_of(p, &lo, &hi)) {
        return false;
    }
    int v = get(p), nv = clampi(v + steps, lo, hi);
    if (nv == v) {
        return false;
    }
    put(p, nv);
    from_unit = false;
    if (p == P_STEP || p == P_LSTART || p == P_LEND) {
        synth_redraw();  /* the ranges and the other columns follow */
    }
    return true;
}

static void param_toggle(int p)
{
    ensure();
    if (p == P_INSERT) {
        from_unit = false;
        insert_step();
        synth_redraw();
    } else if (p == P_DELETE) {
        from_unit = false;
        delete_step();
        synth_redraw();
    } else if (p == P_BACKFORTH) {
        put(p, !get(p));
        from_unit = false;
    }
}

static bool param_reset(int p)
{
    ensure();
    int lo, hi;
    if (is_step_param(p) && p == P_STEP) {
        return false;
    }
    if (!range_of(p, &lo, &hi) || get(p) == clampi(default_of(p), lo, hi)) {
        return false;
    }
    put(p, clampi(default_of(p), lo, hi));
    from_unit = false;
    return true;
}

/* The wave's number and name, as in the PATCH entry (a card has no names) */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    if (p != P_WAVE) {
        return SYNTH_DRAW_NONE;
    }
    ensure();
    if (length() == 0) {
        return SYNTH_DRAW_NONE;
    }
    char code[8], name[WS_WAVE_NAME_LEN + 1] = "";
    int v = get(P_WAVE);
    if (!get(P_BANK)) {
        ws_wave_name(3, v + 32, name, sizeof(name));  /* any ROM bank: the PCM names are the same */
    }
    snprintf(code, sizeof(code), "%d", v + 1);
    jv880_draw_code_name(disp, x, y, w, code, name);
    return SYNTH_DRAW_FULL;
}

/* ---- the step's picture and the Step arrows ---- */

static void header_tag(char *buf, size_t len)
{
    if (length()) {
        snprintf(buf, len, "STEP %d/%d", cur + 1, length());
    } else {
        snprintf(buf, len, "NO STEPS");
    }
}

static void pick_step(int d)
{
    ensure();
    int n = clampi(cur + d, 0, length() ? length() - 1 : 0);
    if (n != cur) {
        cur = n;
        synth_redraw();
    }
}

static void instance_step(int d) { pick_step(d > 0 ? 1 : -1); }
static void shift_step(int d) { pick_step(d > 0 ? 8 : -8); }
static int play_channel(void) { return channel(); }
/* ---- listening: Edisyn's test performance (as the PATCH entry), part 1 playing a patch whose wave A is this
 * sequence (wave number 0-31 of its bank) - the sequence as the SR has it, not our edits. Everything goes to the
 * edit buffer (RAM 1 #0 is only the name it arrives under); nothing is written to the unit's memory. ---- */

#define PATCH_LEN 426
#define PRM_CURRENT_BANK 0
#define PRM_CURRENT_PROG 2
#define PRM_MIDI_MODE 4
#define PRM_PART_PATCH_BANK 57
#define PRM_PART_PATCH_NUM 58
#define PRM_PERF_MIDI_MODE 427
#define WS_WAVE_A 90      /* the patch's wave A: bank (low bits) at +2, number (16 bits) at +3, expansion at +83 */
#define WS_BANK_EXP 88

static int aud_bank = -1, aud_num;  /* the SR's sequence this one is, -1 = none */
static bool aud_pending;
static uint8_t aud_buf[900];
#define LOAD_SCREEN_US 3000000  /* the loading screen stays at least this long */
static int64_t aud_t0;

static void audition(int bank, int num)
{
    aud_bank = bank;
    aud_num = num;
    aud_pending = bank >= 0;
    aud_t0 = esp_timer_get_time();
}

static size_t build_dump(uint8_t type, const uint8_t *data, size_t len)
{
    size_t n = 0;
    int sum = 0;
    aud_buf[n++] = 0xF0;
    aud_buf[n++] = 0x42;
    aud_buf[n++] = 0x30 + channel();
    aud_buf[n++] = 0x28;
    aud_buf[n++] = type;
    aud_buf[n++] = 0;  /* RAM 1 */
    aud_buf[n++] = 0;  /* #0 */
    for (size_t i = 0; i < len; i++) {
        aud_buf[n++] = data[i] & 0x0F;
        aud_buf[n++] = data[i] >> 4;
        sum += (data[i] & 0x0F) + (data[i] >> 4);
    }
    aud_buf[n++] = sum & 0x7F;
    aud_buf[n++] = 0xF7;
    return n;
}

/* The PATCH entry's Init patch with wave A = the sequence */
static size_t build_audition_patch(void)
{
    uint8_t p[PATCH_LEN] = {0};
    FILE *f = fopen(STORAGE_ROOT "/wsp/000 Init", "rb");
    if (!f || fread(p, 1, PATCH_LEN, f) != PATCH_LEN) {
        memset(p, 0, PATCH_LEN);
    }
    if (f) {
        fclose(f);
    }
    int x = ws_to_ws_exp_bank[aud_bank];
    p[WS_WAVE_A + 2] = x % 4;
    p[WS_BANK_EXP] = (p[WS_BANK_EXP] & ~0x11) | (x >= 8 ? 16 : x >= 4 ? 1 : 0);
    p[WS_WAVE_A + 3] = 0;
    p[WS_WAVE_A + 4] = aud_num;
    p[WS_WAVE_A + 83] = 0;
    return build_dump(0x40, p, PATCH_LEN);
}

static size_t build_test_performance(void)
{
    uint8_t perf[181] = {0};
    memcpy(perf, "Edit            ", 16);
    perf[16] = 2;           /* FX 1: no effect, parallel */
    perf[17] = 2 | 128;     /* FX 2: no effect; extended effect mode (the one Edisyn writes) */
    perf[18] = 6 | 6 << 4;  /* mix 3 and 4: 5/5 */
    for (int pt = 0; pt < 8; pt++) {
        uint8_t *q = perf + 37 + 18 * pt;
        q[0] = 0;                        /* the part's patch: RAM 1 */
        q[1] = pt == 0 ? 0 : 0xFF;       /* #0; -1 = off */
        q[2] = 99;                       /* volume */
        q[3] = 50;                       /* FX bus: A/B 50/50 */
        q[4] = 3 << 4 | 1 << 2 | 2;      /* play: both, polyphonic, last note */
        q[6] = 127;                      /* key range */
        q[7] = 1;                        /* velocity range */
        q[8] = 127;
        q[15] = 1;                       /* sustain pedal */
    }
    return build_dump(0x49, perf, sizeof(perf));
}

static bool send_param(int param, int value)
{
    char text[12];
    snprintf(text, sizeof(text), value > 0 ? "+%d" : "%d", value);
    uint8_t msg[40];
    int n = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x42;
    msg[n++] = 0x30 + channel();
    msg[n++] = 0x28;
    msg[n++] = param >= 407 ? 0x43 : param >= 380 ? 0x42 : 0x41;
    msg[n++] = param & 127;
    msg[n++] = param >> 7 & 127;
    for (const char *c = text; *c; c++) {
        msg[n++] = *c;
    }
    msg[n++] = 0x00;
    msg[n++] = 0xF7;
    return midi_send_sysex(msg, n);
}

/* The PATCH entry's order and pauses (the SR needs them): MIDI mode, performance RAM 1 #0, the test performance,
 * part 1 = RAM 1 #0, the patch as a dump */
typedef enum { S_PARAM, S_CC, S_PC, S_PERF, S_PATCH } step_kind_t;
typedef struct {
    uint8_t kind;
    uint16_t param;
    int16_t value;
    uint16_t gap_ms;
} step_t;
static const step_t aud_steps[] = {
    {S_PARAM, PRM_MIDI_MODE, 1, 40},
    {S_PARAM, PRM_MIDI_MODE, 0, 40},
    {S_CC, 0, 0, 20},
    {S_CC, 32, 0, 20},
    {S_PC, 0, 0, 20},
    {S_PARAM, PRM_CURRENT_BANK, 0, 40},
    {S_PARAM, PRM_CURRENT_PROG, 0, 300},
    {S_PERF, 0, 0, 300},
    {S_PARAM, PRM_MIDI_MODE, 1, 40},
    {S_PARAM, PRM_MIDI_MODE, 0, 40},
    {S_PARAM, PRM_PART_PATCH_BANK, 0, 40},
    {S_PARAM, PRM_PART_PATCH_NUM, 0, 300},
    {S_PARAM, PRM_MIDI_MODE, 1, 40},  /* changing the patch turns on OMNI: off again */
    {S_PARAM, PRM_PERF_MIDI_MODE, 1, 40},
    {S_PATCH, 0, 0, 300},
};
#define AUD_STEPS (sizeof(aud_steps) / sizeof(aud_steps[0]))
static int aud_i = -1;  /* the step to send next, -1 = not running */
static int64_t aud_next_us;

static bool send_step(const step_t *st)
{
    switch (st->kind) {
    case S_PARAM:
        return send_param(st->param, st->value);
    case S_CC:
        midi_send(0xB0 | channel(), st->param, st->value);
        return true;
    case S_PC:
        midi_send(0xC0 | channel(), st->param, 0);
        return true;
    case S_PERF:
        return midi_send_sysex(aud_buf, build_test_performance());
    default:
        return midi_send_sysex(aud_buf, build_audition_patch());
    }
}

/* While the SR is being set up (the messages take 1.6 s, the screen stays 3 s) the editor takes no input
 * (synth_t.busy): a second load in the middle would start the sequence over */
static bool busy(void)
{
    return ws_banks_busy() || aud_pending || aud_i >= 0 ||
           (aud_bank >= 0 && esp_timer_get_time() < aud_t0 + LOAD_SCREEN_US);
}

static void busy_draw(int disp)
{
    if (ws_banks_busy()) {
        ws_banks_draw(disp);
        return;
    }
    int64_t el = esp_timer_get_time() - aud_t0;
    int done = el <= 0 ? 0 : el >= LOAD_SCREEN_US ? 1000 : (int)(el * 1000 / LOAD_SCREEN_US);
    if (disp == 0) {
        display_text(0, 1, 0, &font_small, "WAVE SEQUENCE", true);
        display_fill_rect(0, 0, 11, 255, 1, true);
        display_text(0, 4, 18, &font_medium, "LOADING", true);
        display_text(0, 4, 42, &font_small, "setting up the SR", true);
    } else {
        display_text(1, 1, 0, &font_small, "do not touch the SR or the knobs", true);
        display_fill_rect(1, 0, 11, 255, 1, true);
        display_rect(1, 6, 18, 242, 18, true);
        display_fill_rect(1, 8, 20, 238 * done / 1000, 14, true);
    }
}

static void audition_tick(int64_t now)
{
    if (aud_pending) {
        aud_pending = false;
        aud_i = 0;  /* (a run in progress starts over) */
    }
    if (aud_i < 0 || now < aud_next_us) {
        return;
    }
    if (send_step(&aud_steps[aud_i])) {
        aud_next_us = now + aud_steps[aud_i].gap_ms * 1000LL;
        if (++aud_i == (int)AUD_STEPS) {
            aud_i = -1;
        }
    }
}

static bool activate(void)
{
    ensure();
    if (aud_bank >= 0) {
        audition(aud_bank, aud_num);
    }
    return false;
}

/* ---- reading the unit (Auto) ---- */

static void read_seq(void)
{
    int b = clampi(synth_setting(SETTING_BANK), 0, 11);
    uint8_t msg[7] = {0xF0, 0x42, 0x30 + channel(), 0x28, ws_kind_seq.request, ws_to_ws_bank[b], 0xF7};
    midi_send_sysex(msg, sizeof(msg));
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (ws_banks_sysex(d, len)) {
        return;  /* a bank dump for a running job, or a status message */
    }
    if (len != (size_t)ws_kind_seq.dump_len || d[1] != 0x42 || d[3] != 0x28 || d[4] != ws_kind_seq.reply) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    int sum = 0;
    for (size_t i = 6; i < len - 2; i++) {
        sum += d[i];
    }
    if ((sum & 0x7F) != d[len - 2]) {
        ESP_LOGW(TAG, "SysEx ignored: bad checksum");
        return;
    }
    ensure();
    static uint8_t raw[WSSEQ_MAX_LEN];
    int n = ws_seq_extract(d + 6, clampi(synth_setting(SETTING_NUM), 0, 31), raw);
    if (n <= 0) {
        synth_notice("THAT SEQUENCE IS EMPTY");
        return;
    }
    memcpy(seq, raw, n);
    memcpy(as_read, seq, n);
    cur = 0;
    from_unit = as_read_ok = true;
    audition(clampi(synth_setting(SETTING_BANK), 0, 11), clampi(synth_setting(SETTING_NUM), 0, 31));
    synth_sound_replaced();
    synth_redraw();
}

static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(seq, as_read, sizeof(seq));
    fix_cur();
    return true;
}

static bool sound_name(char *buf, size_t len)
{
    if (!from_unit) {
        return false;
    }
    snprintf(buf, len, "%.*s", NAME_CHARS, (const char *)seq);
    for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) {
        buf[i] = 0;
    }
    return true;
}

static void tick(int64_t now)
{
    if (ws_banks_busy()) {
        ws_banks_tick(now);
        return;
    }
    audition_tick(now);
}

/* ---- sequences (library) ---- */

static bool patch_load(const char *path)
{
    ensure();
    static uint8_t buf[WSSEQ_MAX_LEN];
    FILE *f = fopen(path, "rb");
    if (!f) {
        size_t n = strlen(path);
        if (n >= 8 && !strcmp(path + n - 8, "000 Init")) {  /* the default patch needs no file */
            memcpy(seq, init_seq, sizeof(seq));
            cur = 0;
            from_unit = as_read_ok = false;
            aud_bank = -1;
            return true;
        }
        return false;
    }
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got < WSSEQ_HEADER || got != (size_t)(WSSEQ_HEADER + buf[OFF_LEN] * WSSEQ_STEP_LEN)) {
        ESP_LOGE(TAG, "can't read %s (%d bytes)", path, (int)got);
        return false;
    }
    memset(seq, 0, sizeof(seq));
    memcpy(seq, buf, got);
    cur = 0;
    from_unit = as_read_ok = false;
    /* an imported one (NNN = bank * 32 + n + 1) is in the SR: listen to it; our own are not */
    const char *base = strrchr(path, '/');
    int n = atoi(base ? base + 1 : path);
    if (n >= 1 && n <= 12 * 32) {
        audition((n - 1) / 32, (n - 1) % 32);
    } else {
        aud_bank = -1;
        synth_notice("NOT IN THE SR: NO SOUND");
    }
    return true;
}

static void set_name(const char *name)
{
    for (int i = 0; i < NAME_LEN; i++) {
        char c = i < NAME_CHARS && *name ? *name++ : ' ';
        seq[i] = c >= 32 && c < 127 ? c : ' ';
    }
}

static bool patch_save(const char *path)
{
    ensure();
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    set_name(sp ? sp + 1 : title);
    mkdir(STORAGE_ROOT "/wsseq", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    size_t n = WSSEQ_HEADER + length() * WSSEQ_STEP_LEN;
    bool ok = fwrite(seq, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* ---- IMPORT ---- */

static void import_start(void)
{
    uint16_t mask;
    switch (synth_setting(SETTING_IMPORT) & 3) {
    case 0: mask = 0x0007; break;  /* RAM 1-3 */
    case 1: mask = 0x07F8; break;  /* ROM 4-11 */
    case 2: mask = 0x0800; break;  /* the card */
    default: mask = 1 << clampi(synth_setting(SETTING_BANK), 0, 11); break;
    }
    ws_banks_import(&ws_kind_seq, mask, channel());
}

/* The header's label for an imported sequence: its place in the SR instead of the library number */
static void patch_label(const char *file, char *buf, size_t len)
{
    int n = atoi(file);
    const char *sp = strchr(file, ' ');
    if (n < 1 || n > 12 * 32 || !sp) {
        snprintf(buf, len, "%s", file);
        return;
    }
    snprintf(buf, len, "%s:%02d%s", ws_bank_tags[(n - 1) / 32], (n - 1) % 32, sp);
}

static int unknown_count(void) { return 0; }

static const synth_tip_t tips[] = {
    {"WAVE SEQUENCE", {"A sequence plays up to 255 steps: a wave,", "level, duration, crossfade, tuning. Pick the", "step with the STEP knob or the STEP arrows",
                       "(SHIFT: 8 steps). Display buttons: INSERT", "copies the step after itself, DELETE removes", "it. B: start, loop, repeats, modulation.",
                       "The SR plays the unedited sequence."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_SHIFT, BTN_GROUP_A, BTN_GROUP_A + 1, -1, -1, -1}},
    {"FROM THE UNIT", {"CONTROL > IMPORT (twice) reads the banks'", "sequences into the library as 1-384", "(bank x 32 + n + 1; ~5 s a bank).",
                       "AUTO reads the READ BANK / READ SEQ one.", "The SR must send on its MIDI OUT to the", "Maschine's MIDI IN.", ""},
     {BTN_CONTROL, BTN_AUTO, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_wsseq = {
    .name = "Wavestation SR SEQ",
    .device = "Wavestation SR",
    .entry = "SEQ",
    .key = "wsseq",
    .patch_dir = STORAGE_ROOT "/wsseq",
    .default_patch = "000 Init",
    .patch_label = patch_label,
    .first_user_patch = 500,
    .pages = pages,
    .page_count = sizeof(pages) / sizeof(pages[0]),
    .settings = settings,
    .setting_count = sizeof(settings) / sizeof(settings[0]),
    .setting_note = setting_note,
    .param_name = param_name,
    .param_format = param_format,
    .param_bar = param_bar,
    .param_on = param_on,
    .param_units = param_units,
    .param_step = param_step,
    .param_toggle = param_toggle,
    .param_reset = param_reset,
    .param_draw = param_draw,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .revert = revert,
    .unknown_count = unknown_count,
    .activate = activate,
    .play_channel = play_channel,
    .instance_step = instance_step,
    .shift_step = shift_step,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_seq,
    .sysex_in = sysex_in,
    .tick = tick,
    .busy = busy,
    .busy_draw = busy_draw,
    .action_name = "IMPORT",
    .action = import_start,
    .theme_bright = {127, 20, 20},  /* red, as the SR's other entries */
    .theme_dim = {14, 2, 2},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
