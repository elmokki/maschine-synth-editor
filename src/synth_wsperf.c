/*
 * Korg Wavestation SR, PERF: a performance is a name, the effects block and eight parts, each part playing a
 * patch of the SR's own memory (bank + number) with its level, FX bus, key and velocity range, tuning and
 * play settings. The patches are edited in the PATCH entry; here they are put together.
 *
 * Layout: A PART (page 1: patch, level, bus, key and velocity range; page 2: tuning, delay, play settings),
 * B FX ROUTING (the two effect types, series / parallel, mixes 3 and 4 with their modulation), C FX 1 and D FX 2
 * (the unit's type and its parameters over two pages: what they are depends on the type).
 * The part is picked with Shift + Group A-H or the transport Step arrows. The effects block (21 bytes: routing and
 * mixes, then 8 bytes per unit, packed by effect group) is edited through the generated tables (ws_fx.c,
 * ws_fx_data.c); a new type starts from its factory preset.
 *
 * The performance goes to the SR as one dump into its edit buffer (RAM 1 #0 selected first, as the PATCH entry's
 * test performance), a little after the last change. Storing goes through the bank dumps (ws_banks.c).
 * A performance file is the performance as the SR keeps it: 181 bytes (layout: Edisyn's
 * KorgWavestationPerformance.java): name 16, effects 21, 8 parts x 18.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "ws_banks.h"
#include "ws_fx.h"

static const char *TAG = "wsperf";

#define PERF_LEN 181
#define NAME_LEN 16
#define NAME_CHARS 15
#define FX_OFFSET 16
#define PART_OFFSET 37
#define PART_LEN 18
#define PARTS 8
#define PERF_BANK_ITEMS 50
#define PATCH_ITEMS 35
#define SEND_DELAY_US 250000  /* after the last change */
#define PRM_CURRENT_BANK 0
#define PRM_CURRENT_PROG 2
#define PRM_MIDI_MODE 4
#define PRM_PERF_MIDI_MODE 427

/* ---- parameters (of the part being edited) ---- */

/* The part's parameters (the part being edited), then the effects block's: routing and mixes, the two units' types,
 * and WS_FX_MAX_PARAMS slots per unit that mean whatever the unit's effect type has there (name, range and text
 * from the generated tables) */
enum {
    P_BANK, P_PATCH, P_VOLUME, P_FXBUS, P_LOKEY, P_HIKEY, P_LOVEL, P_HIVEL, P_TRANS, P_DETUNE, P_DELAY, P_SCALE,
    P_SCALEKEY, P_PLAY, P_VOICE, P_PRIO, P_SUSTAIN,
    P_SERIES, P_MIX3, P_MOD3SRC, P_MOD3AMT, P_MIX4, P_MOD4SRC, P_MOD4AMT, P_FX1_TYPE, P_FX2_TYPE,
    P_FX_FIRST,
    P_FX1_0 = P_FX_FIRST,
    P_FX2_0 = P_FX1_0 + WS_FX_MAX_PARAMS,
    P_COUNT = P_FX2_0 + WS_FX_MAX_PARAMS
};
#define P_FX_NAMED P_FX_FIRST  /* the ones with a fixed name in pinfo */

typedef struct {
    const char *name;
    int16_t min, max, def;
} pinfo_t;

static const pinfo_t pinfo[P_FX_NAMED] = {
    [P_BANK] = {"BANK", 0, 11, 0},
    [P_PATCH] = {"PATCH", -1, 34, 0},
    [P_VOLUME] = {"VOLUME", 0, 99, 99},
    [P_FXBUS] = {"FX BUS", 0, 105, 50},
    [P_LOKEY] = {"LOW KEY", 0, 127, 0},
    [P_HIKEY] = {"HIGH KEY", 0, 127, 127},
    [P_LOVEL] = {"LOW VEL", 1, 127, 1},
    [P_HIVEL] = {"HIGH VEL", 1, 127, 127},
    [P_TRANS] = {"TRANSPOSE", -24, 24, 0},
    [P_DETUNE] = {"DETUNE", -49, 49, 0},
    [P_DELAY] = {"DELAY ms", 0, 9999, 0},
    [P_SCALE] = {"SCALE", 0, 15, 0},
    [P_SCALEKEY] = {"SCALE KEY", 1, 12, 1},
    [P_PLAY] = {"PLAY MODE", 0, 2, 2},
    [P_VOICE] = {"VOICE", 0, 2, 0},
    [P_PRIO] = {"PRIORITY", 0, 2, 2},
    [P_SUSTAIN] = {"SUSTAIN", 0, 1, 1},
    [P_SERIES] = {"SERIES", 0, 1, 0},
    [P_MIX3] = {"MIX 3", 0, 11, 1},
    [P_MOD3SRC] = {"MOD 3 SRC", 0, 14, 0},
    [P_MOD3AMT] = {"MOD 3 AMT", -15, 15, 0},
    [P_MIX4] = {"MIX 4", 0, 11, 11},
    [P_MOD4SRC] = {"MOD 4 SRC", 0, 14, 0},
    [P_MOD4AMT] = {"MOD 4 AMT", -15, 15, 0},
    [P_FX1_TYPE] = {"FX 1 TYPE", 0, WS_FX_TYPES - 1, 0},
    [P_FX2_TYPE] = {"FX 2 TYPE", 0, WS_FX_TYPES - 1, 0},
};

/* Mix 3 and 4 name the same 0-11 differently when the effects run in series (dry...wet) or in parallel (left...right) */
static const char *const mix_parallel[12] = {"OFF", "LEFT", "9/1", "8/2", "7/3", "6/4", "5/5", "4/6", "3/7", "2/8", "1/9", "RIGHT"};
static const char *const mix_series[12] = {"OFF", "DRY", "9/1", "8/2", "7/3", "6/4", "5/5", "4/6", "3/7", "2/8", "1/9", "WET"};

static const char *const scale_names[16] = {"EQUAL 1", "EQUAL 2", "PURE MAJ", "PURE MIN", "USER 1", "USER 2",
                                            "USER 3", "USER 4", "USER 5", "USER 6", "USER 7", "USER 8",
                                            "USER 9", "USER 10", "USER 11", "USER 12"};
static const char *const play_names[3] = {"LOCAL", "MIDI", "LOCAL+MIDI"};
static const char *const voice_names[3] = {"POLY", "UNI RETRIG", "UNI LEGATO"};
static const char *const prio_names[3] = {"LOW NOTE", "HIGH NOTE", "LAST NOTE"};
static const char *const high_buses[6] = {"B", "C", "C+D", "D", "ALL", "PATCH"};
static const char *const key_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

#define _ SYNTH_NONE
#define NO_SW {_, _, _, _, _, _, _, _}
enum { PG_PART, PG_PART2, PG_ROUTE, PG_FX1, PG_FX1B, PG_FX2, PG_FX2B };
#define FXP(unit, n) (P_FX##unit##_0 + (n))
static const synth_page_t pages[] = {
    [PG_PART] = {0, "PART", {P_BANK, P_PATCH, P_VOLUME, P_FXBUS, P_LOKEY, P_HIKEY, P_LOVEL, P_HIVEL}, NO_SW},
    [PG_PART2] = {0, "PART 2", {P_TRANS, P_DETUNE, P_DELAY, P_SCALE, P_SCALEKEY, P_PLAY, P_VOICE, P_PRIO},
                  {P_SUSTAIN, _, _, _, _, _, _, _}},
    /* B: the two effect units and how they meet (the switch sits above Mix 3, whose names it changes) */
    [PG_ROUTE] = {1, "FX ROUTING", {P_FX1_TYPE, P_FX2_TYPE, P_MIX3, P_MIX4, P_MOD3SRC, P_MOD3AMT, P_MOD4SRC, P_MOD4AMT},
                  {_, _, P_SERIES, _, _, _, _, _}},
    /* C, D: one unit each, the same two pages (parallel: the page stays when switching) */
    [PG_FX1] = {2, "FX 1", {P_FX1_TYPE, FXP(1, 0), FXP(1, 1), FXP(1, 2), FXP(1, 3), FXP(1, 4), FXP(1, 5), FXP(1, 6)},
                NO_SW, 1},
    [PG_FX1B] = {2, "FX 1 (2)", {FXP(1, 7), FXP(1, 8), FXP(1, 9), FXP(1, 10), FXP(1, 11), _, _, _}, NO_SW, 1},
    [PG_FX2] = {3, "FX 2", {P_FX2_TYPE, FXP(2, 0), FXP(2, 1), FXP(2, 2), FXP(2, 3), FXP(2, 4), FXP(2, 5), FXP(2, 6)},
                NO_SW, 1},
    [PG_FX2B] = {3, "FX 2 (2)", {FXP(2, 7), FXP(2, 8), FXP(2, 9), FXP(2, 10), FXP(2, 11), _, _, _}, NO_SW, 1},
};
#undef _

/* ---- settings ---- */

#define SETTING_BANK 1
#define SETTING_NUM 2
#define SETTING_IMPORT 3
static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    /* The performance AUTO reads: bank 0-11 (RAM 1-3, ROM 4-11, Card), number 0-49 */
    [SETTING_BANK] = {"READ BANK", SET_NUMBER, 0, 11, 0},
    [SETTING_NUM] = {"READ PERF", SET_NUMBER, 0, 49, 0},
    /* What IMPORT reads from the SR into the library: its banks, 50 performances each */
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

static uint8_t perf[PERF_LEN], init_perf[PERF_LEN], as_read[PERF_LEN];
static bool loaded, from_unit, as_read_ok;
static int part;
static bool perf_dirty;
static int64_t send_at, next_send_us;

static void ensure(void)
{
    if (loaded) {
        return;
    }
    loaded = true;
    FILE *f = fopen(STORAGE_ROOT "/wsperf/000 Init", "rb");
    if (f) {
        if (fread(init_perf, 1, PERF_LEN, f) != PERF_LEN) {
            memset(init_perf, 0, PERF_LEN);
        }
        fclose(f);
    }
    memcpy(perf, init_perf, PERF_LEN);
}

static int channel(void) { return synth_setting(SETTING_CHANNEL) & 0x0F; }

static uint8_t *part_bytes(int pt) { return perf + PART_OFFSET + PART_LEN * pt; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A part's byte layout: 0 bank (low 2 bits of the expanded number), 1 patch (-1 = off), 2 level, 3 FX bus,
 * 4 mode (bits 7-6 bank high, 5-4 play, 3-2 voice, 1-0 priority), 5-6 key range, 7-8 velocity range,
 * 9 transpose, 10 detune, 11 scale, 12 scale key, 13-14 MIDI out (unused), 15 sustain, 16-17 delay (big-endian) */
static int get(int p, int pt)
{
    const uint8_t *q = part_bytes(pt);
    int v;
    switch (p) {
    case P_BANK: v = ws_from_ws_exp_bank[((q[4] >> 6 & 3) * 4 + (q[0] & 3)) % 12]; break;
    case P_PATCH: v = (int8_t)q[1]; break;
    case P_VOLUME: v = q[2]; break;
    case P_FXBUS: v = q[3]; break;
    case P_LOKEY: v = q[5]; break;
    case P_HIKEY: v = q[6]; break;
    case P_LOVEL: v = q[7]; break;
    case P_HIVEL: v = q[8]; break;
    case P_TRANS: v = (int8_t)q[9]; break;
    case P_DETUNE: v = (int8_t)q[10]; break;
    case P_DELAY: v = q[16] << 8 | q[17]; break;
    case P_SCALE: v = q[11]; break;
    case P_SCALEKEY: v = q[12] + 1; break;
    case P_PLAY: v = (q[4] >> 4 & 3) - 1; break;
    case P_VOICE: v = (q[4] >> 2 & 3) - 1; break;
    case P_PRIO: v = q[4] & 3; break;
    default: v = q[15] != 0; break;
    }
    return clampi(v, pinfo[p].min, pinfo[p].max);
}

static void set(int p, int pt, int v)
{
    uint8_t *q = part_bytes(pt);
    v = clampi(v, pinfo[p].min, pinfo[p].max);
    switch (p) {
    case P_BANK: {
        int exp = ws_to_ws_exp_bank[v];
        q[0] = exp % 4;
        q[4] = (q[4] & 0x3F) | (exp / 4 & 3) << 6;
        break;
    }
    case P_PATCH: q[1] = (uint8_t)v; break;
    case P_VOLUME: q[2] = v; break;
    case P_FXBUS: q[3] = v; break;
    case P_LOKEY: q[5] = v; break;
    case P_HIKEY: q[6] = v; break;
    case P_LOVEL: q[7] = v; break;
    case P_HIVEL: q[8] = v; break;
    case P_TRANS: q[9] = (uint8_t)v; break;
    case P_DETUNE: q[10] = (uint8_t)v; break;
    case P_DELAY: q[16] = v >> 8; q[17] = v & 0xFF; break;
    case P_SCALE: q[11] = v; break;
    case P_SCALEKEY: q[12] = v - 1; break;
    case P_PLAY: q[4] = (q[4] & ~0x30) | (v + 1) << 4; break;
    case P_VOICE: q[4] = (q[4] & ~0x0C) | (v + 1) << 2; break;
    case P_PRIO: q[4] = (q[4] & ~0x03) | v; break;
    default: q[15] = v; break;
    }
}

/* ---- the effects block ---- */

static uint8_t *fx_block(void) { return perf + FX_OFFSET; }

static int fx_unit_of(int p) { return (p - P_FX_FIRST) / WS_FX_MAX_PARAMS + 1; }

static uint8_t *fx_unit_bytes(int unit) { return fx_block() + (unit == 1 ? 5 : 13); }

/* The Stereo Vocoder effects (the last two types) take up both effect slots: "when you select one of the Stereo
 * Vocoders for Effect 1 or 2, the other Effect changes to display Stereo Vocoder as well" (Reference Guide, FX
 * SELECT, FX 1/2). So both units get the type, and FX 2 follows FX 1 (can't be changed on its own) */
#define FIRST_STEREO_VOCODER 54

static bool fx_locked(int unit) { return unit == 2 && ws_fx_type_get(fx_block(), 1) >= FIRST_STEREO_VOCODER; }

/* What a slot means now: the effect group's parameter, NULL when the type has fewer */
static const ws_fx_param_t *fx_param(int p)
{
    if (p < P_FX_FIRST || fx_locked(fx_unit_of(p))) {
        return NULL;
    }
    int unit = fx_unit_of(p), slot = (p - P_FX_FIRST) % WS_FX_MAX_PARAMS;
    const ws_fx_group_t *g = &ws_fx_groups[ws_fx_type_group[ws_fx_type_get(fx_block(), unit)]];
    return slot < g->nparams ? &g->params[slot] : NULL;
}

/* Routing, mixes and types: Mix 3/4 are the low/high nibble of byte 2, their sources of byte 3, their amounts' size
 * of byte 4 and the sign bit 6 of byte 0/1; the series flag is bit 7 of byte 0 */
static int fx_get(int p)
{
    const uint8_t *b = fx_block();
    int v;
    switch (p) {
    case P_SERIES: v = b[0] >> 7; break;
    case P_MIX3: v = b[2] & 15; break;
    case P_MIX4: v = b[2] >> 4; break;
    case P_MOD3SRC: v = b[3] & 15; break;
    case P_MOD4SRC: v = b[3] >> 4; break;
    case P_MOD3AMT: v = b[0] & 0x40 ? -(b[4] & 15) : b[4] & 15; break;
    case P_MOD4AMT: v = b[1] & 0x40 ? -(b[4] >> 4) : b[4] >> 4; break;
    case P_FX1_TYPE: return ws_fx_type_get(b, 1);
    case P_FX2_TYPE: return ws_fx_type_get(b, 2);
    default: {
        const ws_fx_param_t *fp = fx_param(p);
        return fp ? ws_fx_get(fx_unit_bytes(fx_unit_of(p)), fp) : 0;
    }
    }
    return clampi(v, pinfo[p].min, pinfo[p].max);
}

static void fx_set(int p, int v)
{
    uint8_t *b = fx_block();
    if (p >= P_FX_FIRST) {
        const ws_fx_param_t *fp = fx_param(p);
        if (fp) {
            ws_fx_put(fx_unit_bytes(fx_unit_of(p)), fp, v);
        }
        return;
    }
    v = clampi(v, pinfo[p].min, pinfo[p].max);
    int mag = v < 0 ? -v : v;
    switch (p) {
    case P_SERIES: b[0] = (b[0] & 0x7F) | v << 7; break;
    case P_MIX3: b[2] = (b[2] & 0xF0) | v; break;
    case P_MIX4: b[2] = (b[2] & 0x0F) | v << 4; break;
    case P_MOD3SRC: b[3] = (b[3] & 0xF0) | v; break;
    case P_MOD4SRC: b[3] = (b[3] & 0x0F) | v << 4; break;
    case P_MOD3AMT:
        b[4] = (b[4] & 0xF0) | mag;
        b[0] = (b[0] & ~0x40) | (v < 0 ? 0x40 : 0);
        break;
    case P_MOD4AMT:
        b[4] = (b[4] & 0x0F) | mag << 4;
        b[1] = (b[1] & ~0x40) | (v < 0 ? 0x40 : 0);
        break;
    default: {
        int unit = p == P_FX1_TYPE ? 1 : 2;
        int old = ws_fx_type_get(b, unit);
        ws_fx_type_set(b, unit, v);
        if (v >= FIRST_STEREO_VOCODER) {
            ws_fx_type_set(b, 3 - unit, v);
        } else if (unit == 1 && old >= FIRST_STEREO_VOCODER) {
            ws_fx_type_set(b, 2, 0);  /* FX 2 was only the vocoder's second half */
        }
        break;
    }
    }
}

/* One accessor for every parameter: the part's (part being edited) or the effects block's */
static int value_of(int p) { return p >= P_SERIES ? fx_get(p) : get(p, part); }

static void value_set(int p, int v)
{
    if (p >= P_SERIES) {
        fx_set(p, v);
    } else {
        set(p, part, v);
    }
}

/* false = no such parameter right now (an effect slot the type doesn't use) */
static bool range_of(int p, int *lo, int *hi)
{
    if ((p == P_FX1_TYPE && fx_locked(1)) || (p == P_FX2_TYPE && fx_locked(2))) {
        return false;
    }
    if (p >= P_FX_FIRST) {
        const ws_fx_param_t *fp = fx_param(p);
        if (!fp) {
            return false;
        }
        *lo = fp->min;
        *hi = fp->max;
        return true;
    }
    *lo = pinfo[p].min;
    *hi = pinfo[p].max;
    return true;
}

/* What the init patch / a fresh effect has there */
static int default_of(int p)
{
    if (p >= P_FX_FIRST) {
        const ws_fx_param_t *fp = fx_param(p);
        return fp ? ws_fx_get(ws_fx_type_preset[ws_fx_type_get(fx_block(), fx_unit_of(p))], fp) : 0;
    }
    if (p == P_MIX3) {
        return fx_get(P_SERIES) ? 11 : 1;
    }
    return pinfo[p].def;
}

/* ---- sending ---- */

static uint8_t dump_buf[400];

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

/* F0 42 3n 28 49 <bank> <num> <181 bytes as nibbles, low then high> <sum of the nibbles> F7: into the edit buffer
 * (RAM 1 #0 is only the name it arrives under) */
static size_t build_dump(void)
{
    size_t n = 0;
    int sum = 0;
    dump_buf[n++] = 0xF0;
    dump_buf[n++] = 0x42;
    dump_buf[n++] = 0x30 + channel();
    dump_buf[n++] = 0x28;
    dump_buf[n++] = 0x49;
    dump_buf[n++] = 0;
    dump_buf[n++] = 0;
    for (int i = 0; i < PERF_LEN; i++) {
        dump_buf[n++] = perf[i] & 0x0F;
        dump_buf[n++] = perf[i] >> 4;
        sum += (perf[i] & 0x0F) + (perf[i] >> 4);
    }
    dump_buf[n++] = sum & 0x7F;
    dump_buf[n++] = 0xF7;
    return n;
}

/* Selecting the synth: Edisyn's sequence up to the performance (RAM 1 #0 in the edit buffer), our performance
 * as the dump, then the performance MIDI mode as the PATCH entry sets it. The pauses are the SR's need. */
typedef enum { S_PARAM, S_CC, S_PC, S_PERF } step_kind_t;
typedef struct {
    uint8_t kind;
    uint16_t param;
    int16_t value;
    uint16_t gap_ms;
} step_t;
static step_t seq[16];
static int seq_n, seq_i;
#define STEP(k, p, v, g) (seq[seq_n++] = (step_t){k, p, v, g})

static void select_performance(void)
{
    seq_n = seq_i = 0;
    STEP(S_PARAM, PRM_MIDI_MODE, 1, 40);
    STEP(S_PARAM, PRM_MIDI_MODE, 0, 40);
    STEP(S_CC, 0, 0, 20);
    STEP(S_CC, 32, 0, 20);
    STEP(S_PC, 0, 0, 20);
    STEP(S_PARAM, PRM_CURRENT_BANK, 0, 40);
    STEP(S_PARAM, PRM_CURRENT_PROG, 0, 300);
    STEP(S_PERF, 0, 0, 300);
    STEP(S_PARAM, PRM_PERF_MIDI_MODE, 1, 40);
}

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
    default:
        return midi_send_sysex(dump_buf, build_dump());
    }
}

static void changed(void)
{
    perf_dirty = true;
    send_at = esp_timer_get_time() + SEND_DELAY_US;
}

static void tick(int64_t now)
{
    if (ws_banks_busy()) {
        ws_banks_tick(now);
        return;
    }
    if (now < next_send_us) {
        return;
    }
    ensure();
    if (seq_i < seq_n) {
        if (send_step(&seq[seq_i])) {
            next_send_us = now + seq[seq_i].gap_ms * 1000LL;
            if (seq[seq_i].kind == S_PERF) {
                perf_dirty = false;  /* it went with the dump */
            }
            seq_i++;
        }
        return;
    }
    if (perf_dirty && now >= send_at) {
        if (midi_send_sysex(dump_buf, build_dump())) {
            perf_dirty = false;
            next_send_us = now + 150000;
        }
    }
}

/* ---- patch names (kept in memory) ---- */

static char factory_names[12][PATCH_ITEMS][16];
static bool names_loaded;

static void load_names(void)
{
    if (names_loaded) {
        return;
    }
    names_loaded = true;
    for (int b = 0; b < 12; b++) {
        char path[40], line[64];
        snprintf(path, sizeof(path), STORAGE_ROOT "/ws/patches/%02d", b);
        FILE *f = fopen(path, "r");
        for (int i = 0; f && i < PATCH_ITEMS && fgets(line, sizeof(line), f); i++) {
            line[strcspn(line, "\r\n")] = 0;
            snprintf(factory_names[b][i], sizeof(factory_names[b][i]), "%s", line);
        }
        if (f) {
            fclose(f);
        }
    }
}

/* What the SR's patch is called: as imported into the library, else as it left the factory (RAM banks may
 * have been written over since) */
static const char *patch_name(int bank, int num)
{
    if (bank < 0 || bank > 11 || num < 0 || num >= PATCH_ITEMS) {
        return "";
    }
    const char *n = ws_banks_lib_name(&ws_kind_patch, bank, num);
    if (n[0]) {
        return n;
    }
    load_names();
    return factory_names[bank][num];
}

static void patch_code(int bank, int num, char *buf, size_t len)
{
    if (num < 0) {
        snprintf(buf, len, "OFF");
    } else {
        snprintf(buf, len, "%s:%02d", ws_bank_tags[bank], num);
    }
}

static void note_text(int n, char *buf, size_t len) { snprintf(buf, len, "%s%d", key_names[n % 12], n / 12 - 1); }

/* ---- params ---- */

/* A new effect type changes what the unit's other columns are */
static bool is_type(int p) { return p == P_FX1_TYPE || p == P_FX2_TYPE; }

static void value_changed(int p)
{
    changed();
    if (is_type(p) || p == P_SERIES) {
        synth_redraw();
    }
}

static bool param_step(int p, int steps)
{
    ensure();
    int lo, hi;
    if (!range_of(p, &lo, &hi)) {
        return false;
    }
    int v = value_of(p), nv = clampi(v + steps, lo, hi);
    if (nv == v) {
        return false;
    }
    value_set(p, nv);
    value_changed(p);
    return true;
}

static void param_toggle(int p)
{
    ensure();
    value_set(p, !value_of(p));
    value_changed(p);
}

static bool param_reset(int p)
{
    ensure();
    int lo, hi;
    if (!range_of(p, &lo, &hi) || value_of(p) == default_of(p)) {
        return false;
    }
    value_set(p, default_of(p));
    value_changed(p);
    return true;
}

static const char *param_name(int p)
{
    if (p >= P_FX_FIRST) {
        ensure();
        const ws_fx_param_t *fp = fx_param(p);
        return fp ? fp->name : "";
    }
    if (p == P_SERIES) {
        ensure();
        return fx_get(P_SERIES) ? "SERIES" : "PARALLEL";
    }
    return pinfo[p].name;
}

static void param_format(int p, char *buf, size_t len)
{
    ensure();
    if (p >= P_FX_FIRST) {
        const ws_fx_param_t *fp = fx_param(p);
        if (fp) {
            ws_fx_format(fp, fx_get(p), buf, len);
        } else {
            buf[0] = 0;
        }
        return;
    }
    int v = value_of(p);
    switch (p) {
    case P_SERIES:
        snprintf(buf, len, v ? "SERIES" : "PARALLEL");
        break;
    case P_MIX3:
    case P_MIX4:
        snprintf(buf, len, "%s", (fx_get(P_SERIES) ? mix_series : mix_parallel)[v]);
        break;
    case P_MOD3SRC:
    case P_MOD4SRC:
        snprintf(buf, len, "%s", ws_fx_tables[0][v]);
        break;
    case P_MOD3AMT:
    case P_MOD4AMT:
        snprintf(buf, len, v ? "%+d" : "0", v);
        break;
    case P_FX1_TYPE:
    case P_FX2_TYPE:
        snprintf(buf, len, "%s", fx_locked(p == P_FX1_TYPE ? 1 : 2) ? "= FX 1" : ws_fx_type_names[v]);
        break;
    case P_BANK:
        snprintf(buf, len, "%s", ws_bank_names[v]);
        break;
    case P_PATCH:
        patch_code(get(P_BANK, part), v, buf, len);
        break;
    case P_FXBUS:
        if (v == 0) {
            snprintf(buf, len, "A");
        } else if (v < 100) {
            snprintf(buf, len, "%d/%d", 100 - v, v);
        } else {
            snprintf(buf, len, "%s", high_buses[v - 100]);
        }
        break;
    case P_LOKEY:
    case P_HIKEY:
        note_text(v, buf, len);
        break;
    case P_TRANS:
    case P_DETUNE:
        snprintf(buf, len, v ? "%+d" : "0", v);
        break;
    case P_SCALE:
        snprintf(buf, len, "%s", scale_names[v]);
        break;
    case P_PLAY:
        snprintf(buf, len, "%s", play_names[v]);
        break;
    case P_VOICE:
        snprintf(buf, len, "%s", voice_names[v]);
        break;
    case P_PRIO:
        snprintf(buf, len, "%s", prio_names[v]);
        break;
    case P_SUSTAIN:
        snprintf(buf, len, v ? "ON" : "OFF");
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
    if (!range_of(p, &lo, &hi) || hi == lo) {
        return -1;
    }
    if (p >= P_FX_FIRST) {
        int fmt = fx_param(p)->fmt;
        if (fmt == WS_FX_CHOICE || fmt == WS_FX_CHECK) {
            return -1;
        }
    }
    if (p == P_BANK || p == P_PATCH || p == P_SCALE || p == P_PLAY || p == P_VOICE || p == P_PRIO || p == P_SUSTAIN ||
        p == P_SERIES || p == P_MOD3SRC || p == P_MOD4SRC || is_type(p)) {
        return -1;
    }
    return (value_of(p) - lo) * 1000 / (hi - lo);
}

static bool param_on(int p)
{
    ensure();
    return (p == P_SUSTAIN && get(P_SUSTAIN, part)) || (p == P_SERIES && fx_get(P_SERIES));
}

static int param_units(int p)
{
    int lo, hi;
    if (!range_of(p, &lo, &hi)) {
        return 80;
    }
    int range = hi - lo;
    if (is_type(p)) {
        return 25;  /* 56 types: one at a time */
    }
    return range <= 1 ? 80 : range <= 6 ? 70 : range <= 16 ? 40 : range <= 30 ? 25 : 10;
}

/* An effect slot the type has no parameter for leaves its column empty */
static bool column_draw(int p, int disp, int x)
{
    (void)disp;
    (void)x;
    if (p < P_FX_FIRST) {
        return false;
    }
    ensure();
    return fx_param(p) == NULL;
}

/* The part's patch: its place big, its name below */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    if (is_type(p)) {
        /* The effect's name, its place in the SR's list of 55 below (the SR itself shows only the name) */
        ensure();
        if (fx_locked(p == P_FX1_TYPE ? 1 : 2)) {
            return SYNTH_DRAW_NONE;
        }
        int type = fx_get(p);
        char num[12];
        const char *name = ws_fx_type_names[type];
        const font_t *f = display_text_width(&font_medium, name) <= w - 2 ? &font_medium : &font_small;
        display_text(disp, x + (w - display_text_width(f, name)) / 2, y + 1 - f->cap_top, f, name, true);
        snprintf(num, sizeof(num), "%d/%d", type, WS_FX_TYPES - 1);
        display_text(disp, x + (w - display_text_width(&font_small, num)) / 2, y + 15 - font_small.cap_top,
                     &font_small, num, true);
        return SYNTH_DRAW_FULL;
    }
    if (p != P_PATCH) {
        return SYNTH_DRAW_NONE;
    }
    ensure();
    int bank = get(P_BANK, part), num = get(P_PATCH, part);
    char code[12];
    patch_code(bank, num, code, sizeof(code));
    jv880_draw_code_name(disp, x, y, w, code, patch_name(bank, num));
    return SYNTH_DRAW_FULL;
}

/* Page 2 and 3 of FX 1 / FX 2 exist only when the effect has that many parameters */
static bool page_enabled(int pg)
{
    if (pg != PG_FX1B && pg != PG_FX2B) {
        return true;
    }
    ensure();
    return fx_param(pg == PG_FX1B ? FXP(1, 7) : FXP(2, 7)) != NULL;
}

/* Patch list (Note Repeat): the part's bank, entry 0 = OFF */
static int patch_list_current(void)
{
    ensure();
    return get(P_PATCH, part) + 1;
}

static void patch_list_name(int i, char *buf, size_t len)
{
    int bank = get(P_BANK, part);
    patch_code(bank, i - 1, buf, len);
    if (i > 0) {
        size_t n = strlen(buf);
        snprintf(buf + n, len - n, " %s", patch_name(bank, i - 1));
    }
}

static void patch_list_pick(int i)
{
    param_step(P_PATCH, i - 1 - get(P_PATCH, part));
}

static const synth_list_t patch_list = {"PATCHES", 36, patch_list_current, patch_list_name, patch_list_pick, P_PATCH};

static const synth_list_t *page_list(int page)
{
    return page == PG_PART ? &patch_list : NULL;
}

/* ---- overview (Grid): the eight parts as a table (no header: eight rows fill the display); the effects as
 * lists, one unit per display; the routing as a picture over both displays ---- */

static void overview_fx_list(int disp)
{
    int unit = disp + 1, type = ws_fx_type_get(fx_block(), unit);
    char buf[48];
    if (fx_locked(unit)) {
        snprintf(buf, sizeof(buf), "FX %d  (stereo vocoder: both slots)", unit);
    } else {
        snprintf(buf, sizeof(buf), "FX %d  %s", unit, ws_fx_type_long[type]);
    }
    display_text(disp, 1, 0, &font_tiny, buf, true);
    display_line(disp, 0, 9, 255, 9, true);
    for (int slot = 0; slot < WS_FX_MAX_PARAMS; slot++) {
        const ws_fx_param_t *fp = fx_param(P_FX_FIRST + (unit - 1) * WS_FX_MAX_PARAMS + slot);
        if (!fp) {
            break;
        }
        int x = slot < 6 ? 1 : 130, y = 12 + (slot % 6) * 8;
        ws_fx_format(fp, ws_fx_get(fx_unit_bytes(unit), fp), buf, sizeof(buf));
        display_text(disp, x, y, &font_tiny, fp->name, true);
        display_text(disp, x + 124 - display_text_width(&font_tiny, buf), y, &font_tiny, buf, true);
    }
}

/* The routing as the Reference Guide's FX MIX page draws it. Buses A/B go into FX 1, C/D into FX 2 (parallel) or past
 * FX 1 (series); Mix 3 / 4 belong to bus C / D (series: how much goes through FX 2, how much straight to the main
 * outputs) or to FX 2's left / right output (parallel: where it is panned on the main outputs). Left display: the
 * picture, right display: what the two mixes do now. */
static void routing_box(int x, int y, int unit)
{
    char t[16];
    display_round_rect(0, x, y, 76, 24, false, true);
    snprintf(t, sizeof(t), "FX %d", unit);
    display_text(0, x + 5, y + 3, &font_tiny, t, true);
    display_text(0, x + 5, y + 13, &font_tiny, ws_fx_type_names[ws_fx_type_get(fx_block(), unit)], true);
}

static void routing_arrow(int x0, int y, int x1)
{
    display_line(0, x0, y, x1, y, true);
    display_line(0, x1 - 4, y - 3, x1, y, true);
    display_line(0, x1 - 4, y + 3, x1, y, true);
}

static void routing_mix_text(int mix, bool series, char *buf, size_t len)
{
    int v = fx_get(mix == 3 ? P_MIX3 : P_MIX4);
    const char *bus = mix == 3 ? "C" : "D";
    if (series) {
        if (v == 0) {
            snprintf(buf, len, "MIX %d OFF: bus %s to out %d only", mix, bus, mix);
        } else if (v == 1) {
            snprintf(buf, len, "MIX %d DRY: bus %s to main, no FX 2", mix, bus);
        } else if (v == 11) {
            snprintf(buf, len, "MIX %d WET: bus %s all via FX 2", mix, bus);
        } else {
            snprintf(buf, len, "MIX %d: bus %s %d direct, %d FX 2", mix, bus, 11 - v, v - 1);
        }
    } else if (v == 0) {
        snprintf(buf, len, "MIX %d OFF: FX 2 %s to out %d only", mix, mix == 3 ? "L" : "R", mix);
    } else if (v == 1) {
        snprintf(buf, len, "MIX %d: FX 2 %s to main 1/L", mix, mix == 3 ? "L" : "R");
    } else if (v == 11) {
        snprintf(buf, len, "MIX %d: FX 2 %s to main 2/R", mix, mix == 3 ? "L" : "R");
    } else {
        snprintf(buf, len, "MIX %d: FX 2 %s pan 1/L %d, 2/R %d", mix, mix == 3 ? "L" : "R", 11 - v, v - 1);
    }
}

static void overview_routing(int disp)
{
    bool series = fx_get(P_SERIES);
    char buf[48];
    if (disp == 1) {
        display_text(1, 1, 0, &font_tiny, series ? "SERIES" : "PARALLEL", true);
        display_line(1, 0, 9, 255, 9, true);
        display_text(1, 1, 13, &font_tiny, series ? "A/B > FX 1 > FX 2 > main 1/L 2/R" : "A/B > FX 1 > main 1/L 2/R", true);
        display_text(1, 1, 22, &font_tiny, series ? "C/D skip FX 1, go to out 3 / 4" : "C/D > FX 2 > out 3 / 4", true);
        routing_mix_text(3, series, buf, sizeof(buf));
        display_text(1, 1, 37, &font_tiny, buf, true);
        routing_mix_text(4, series, buf, sizeof(buf));
        display_text(1, 1, 46, &font_tiny, buf, true);
        return;
    }
    display_text(0, 2, 13, &font_tiny, "A/B", true);
    routing_arrow(20, 16, 28);
    routing_box(28, 4, 1);
    if (series) {
        routing_arrow(104, 16, 118);
        routing_box(118, 4, 2);
        routing_arrow(194, 16, 210);
        display_text(0, 212, 9, &font_tiny, "1/L", true);
        display_text(0, 212, 18, &font_tiny, "2/R", true);
        display_text(0, 2, 45, &font_tiny, "C/D", true);
        display_line(0, 20, 48, 60, 48, true);
        display_round_rect(0, 60, 40, 58, 16, false, true);
        display_text(0, 65, 44, &font_tiny, "MIX 3/4", true);
        routing_arrow(118, 48, 236);
        display_line(0, 156, 48, 156, 28, true);  /* up into FX 2 */
        display_line(0, 153, 32, 156, 28, true);
        display_line(0, 159, 32, 156, 28, true);
        display_text(0, 168, 51, &font_tiny, "direct", true);
        return;
    }
    routing_arrow(104, 16, 210);
    display_text(0, 212, 9, &font_tiny, "1/L", true);
    display_text(0, 212, 18, &font_tiny, "2/R", true);
    display_text(0, 2, 45, &font_tiny, "C/D", true);
    routing_arrow(20, 48, 28);
    routing_box(28, 36, 2);
    routing_arrow(104, 44, 150);
    display_round_rect(0, 150, 36, 58, 16, false, true);
    display_text(0, 155, 40, &font_tiny, "MIX 3/4", true);
    display_text(0, 212, 51, &font_tiny, "3 4", true);
    routing_arrow(104, 56, 200);
    display_line(0, 208, 44, 222, 44, true);
    display_line(0, 222, 44, 222, 27, true);  /* panned to the main outputs */
    display_line(0, 219, 31, 222, 27, true);
    display_line(0, 225, 31, 222, 27, true);
}

static void overview_draw(int page, int disp)
{
    ensure();
    if (page == 1) {
        overview_fx_list(disp);
        return;
    }
    if (page == 2) {
        overview_routing(disp);
        return;
    }
    char buf[80], code[12], lo[8], hi[8], bus[10];
    for (int pt = 0; pt < PARTS; pt++) {
        char mark = pt == part ? '>' : ' ';
        int bank = get(P_BANK, pt), num = get(P_PATCH, pt);
        if (disp == 0) {
            if (num < 0) {
                snprintf(buf, sizeof(buf), "%c%d --", mark, pt + 1);
            } else {
                patch_code(bank, num, code, sizeof(code));
                snprintf(buf, sizeof(buf), "%c%d %-7s %-15.15s LV%3d", mark, pt + 1, code, patch_name(bank, num),
                         get(P_VOLUME, pt));
            }
        } else if (num >= 0) {
            int b = get(P_FXBUS, pt);
            if (b == 0) {
                snprintf(bus, sizeof(bus), "A");
            } else if (b < 100) {
                snprintf(bus, sizeof(bus), "%d/%d", 100 - b, b);
            } else {
                snprintf(bus, sizeof(bus), "%s", high_buses[b - 100]);
            }
            note_text(get(P_LOKEY, pt), lo, sizeof(lo));
            note_text(get(P_HIKEY, pt), hi, sizeof(hi));
            snprintf(buf, sizeof(buf), "%s-%s V%d-%d TR%+d DT%+d BUS %s", lo, hi, get(P_LOVEL, pt),
                     get(P_HIVEL, pt), get(P_TRANS, pt), get(P_DETUNE, pt), bus);
        } else {
            buf[0] = 0;
        }
        display_text(disp, 1, pt * 8, &font_tiny, buf, true);
    }
}

/* ---- parts ---- */

static void select_part(int pt)
{
    part = (pt + PARTS) % PARTS;
    synth_redraw();
}

static void part_step(int d) { select_part(part + (d > 0 ? 1 : -1)); }

/* Shift + Group A-H: part 1-8 - the Group buttons are sections here (only A is used so far) */
static bool shift_group(int g)
{
    select_part(g);
    return true;
}

static int shift_group_led(int g) { return g == part; }

static void header_tag(char *buf, size_t len) { snprintf(buf, len, "PART %d", part + 1); }

static int play_channel(void) { return channel(); }

static bool activate(void)
{
    ensure();
    select_performance();
    return false;
}

/* ---- reading the unit's performance (Auto) ---- */

static void read_perf(void)
{
    int b = clampi(synth_setting(SETTING_BANK), 0, 11);
    uint8_t msg[8] = {0xF0, 0x42, 0x30 + channel(), 0x28, 0x19, ws_to_ws_bank[b],
                      (uint8_t)clampi(synth_setting(SETTING_NUM), 0, 49), 0xF7};
    if (midi_send_sysex(msg, sizeof(msg))) {
        ws_banks_read_begin("PERFORMANCE");
    }
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (ws_banks_sysex(d, len)) {
        return;  /* a bank dump or a status message: the job's */
    }
    /* F0 42 3n 28 49 bank num <362 nibbles> sum F7 */
    if (len != 371 || d[1] != 0x42 || d[3] != 0x28 || d[4] != 0x49) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    int sum = 0;
    for (int i = 7; i < 7 + 2 * PERF_LEN; i++) {
        sum += d[i];
    }
    if ((sum & 0x7F) != d[len - 2]) {
        ESP_LOGW(TAG, "SysEx ignored: bad checksum");
        return;
    }
    ensure();
    ws_banks_read_end();
    for (int i = 0; i < PERF_LEN; i++) {
        perf[i] = d[7 + 2 * i] | d[8 + 2 * i] << 4;
    }
    perf_dirty = false;  /* what was still to send is the unit's now */
    from_unit = true;
    memcpy(as_read, perf, PERF_LEN);
    as_read_ok = true;
    synth_sound_replaced();
    synth_redraw();
}

static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(perf, as_read, PERF_LEN);
    changed();
    return true;
}

static bool sound_name(char *buf, size_t len)
{
    if (!from_unit) {
        return false;
    }
    snprintf(buf, len, "%.*s", NAME_CHARS, (const char *)perf);
    for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) {
        buf[i] = 0;
    }
    return true;
}

/* ---- performances (library) ---- */

static bool patch_load(const char *path)
{
    ensure();
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[PERF_LEN];
    bool ok = fread(buf, 1, PERF_LEN, f) == PERF_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memcpy(perf, buf, PERF_LEN);
    from_unit = as_read_ok = false;
    changed();
    return true;
}

static void set_name(const char *name)
{
    for (int i = 0; i < NAME_LEN; i++) {
        char c = i < NAME_CHARS && *name ? *name++ : ' ';
        perf[i] = c >= 32 && c < 127 ? c : ' ';
    }
}

static bool patch_save(const char *path)
{
    ensure();
    /* The performance's own name (15 characters) follows the file's: "NNN Name" */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    set_name(sp ? sp + 1 : title);
    mkdir(STORAGE_ROOT "/wsperf", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(perf, 1, PERF_LEN, f) == PERF_LEN;
    fclose(f);
    return ok;
}

/* ---- IMPORT and REC > TO SR ---- */

static const uint8_t writable_banks[4] = {0, 1, 2, 11};

static void import_start(void)
{
    uint16_t mask;
    switch (synth_setting(SETTING_IMPORT) & 3) {
    case 0: mask = 0x0007; break;  /* RAM 1-3 */
    case 1: mask = 0x07F8; break;  /* ROM 4-11 */
    case 2: mask = 0x0800; break;  /* the card */
    default: mask = 1 << clampi(synth_setting(SETTING_BANK), 0, 11); break;
    }
    ws_banks_import(&ws_kind_perf, mask, channel());
}

static void unit_slot_name(int slot, char *buf, size_t len)
{
    slot = clampi(slot, 0, 4 * PERF_BANK_ITEMS - 1);
    snprintf(buf, len, "%s:%02d", ws_bank_tags[writable_banks[slot / PERF_BANK_ITEMS]], slot % PERF_BANK_ITEMS);
}

static void unit_slot_info(int slot, char *buf, size_t len)
{
    slot = clampi(slot, 0, 4 * PERF_BANK_ITEMS - 1);
    snprintf(buf, len, "%.12s",
             ws_banks_lib_name(&ws_kind_perf, writable_banks[slot / PERF_BANK_ITEMS], slot % PERF_BANK_ITEMS));
}

/* The slot's bank is read, the performance put in it and the bank sent back */
static void unit_store(int slot, const char *name)
{
    if (ws_banks_busy()) {
        return;
    }
    ensure();
    set_name(name);
    slot = clampi(slot, 0, 4 * PERF_BANK_ITEMS - 1);
    ws_banks_store(&ws_kind_perf, writable_banks[slot / PERF_BANK_ITEMS], slot % PERF_BANK_ITEMS, perf, channel());
}

/* The header's label for an imported performance: its place in the SR instead of the library number */
static void patch_label(const char *file, char *buf, size_t len)
{
    int n = atoi(file);
    const char *sp = strchr(file, ' ');
    if (n < 1 || n > 12 * PERF_BANK_ITEMS || !sp) {
        snprintf(buf, len, "%s", file);
        return;
    }
    snprintf(buf, len, "%s:%02d%s", ws_bank_tags[(n - 1) / PERF_BANK_ITEMS], (n - 1) % PERF_BANK_ITEMS, sp);
}

static int unknown_count(void) { return 0; }

static const synth_tip_t tips[] = {
    {"PERFORMANCE", {"A performance = 8 parts, each playing a", "patch of the SR's memory (bank + number).",
                     "Pick the part with SHIFT + A-H or the", "STEP arrows. A PART: patch, level, bus,",
                     "key and velocity range; page 2: tuning,", "delay, play settings. B: effect types,",
                     "routing, mixes; C / D: FX 1 / FX 2 settings."},
     {BTN_GROUP_A, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_GROUP_A + 1, BTN_GROUP_A + 2, BTN_GROUP_A + 3, -1}},
    {"TO THE UNIT", {"The performance goes to the SR as one", "dump (edit buffer), a moment after the", "last change. AUTO reads the one READ",
                     "BANK / READ PERF name (CONTROL).", "CONTROL > IMPORT (twice) reads the banks", "as library 1-600 (bank x 50 + n + 1).",
                     "REC > TO SR: SR PROTECT INT must be OFF."},
     {BTN_CONTROL, BTN_AUTO, BTN_REC, -1, -1, -1, -1, -1}},
};

const synth_t synth_wsperf = {
    .name = "Wavestation SR PERF",
    .device = "Wavestation SR",
    .entry = "PERF",
    .key = "wsperf",
    .patch_dir = STORAGE_ROOT "/wsperf",
    .default_patch = "000 Init",
    .patch_label = patch_label,
    .first_user_patch = 700,
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
    .page_list = page_list,
    .column_draw = column_draw,
    .page_enabled = page_enabled,
    .overview_pages = 3,
    .overview_draw = overview_draw,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .revert = revert,
    .unknown_count = unknown_count,
    .activate = activate,
    .play_channel = play_channel,
    .instance_step = part_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_perf,
    .sysex_in = sysex_in,
    .tick = tick,
    .busy = ws_banks_busy,
    .busy_draw = ws_banks_draw,
    .action_name = "IMPORT",
    .action = import_start,
    .action_setting = SETTING_IMPORT,
    .unit_store_label = "TO SR",
    .unit_store_slots = 4 * PERF_BANK_ITEMS,
    .unit_slot_name = unit_slot_name,
    .unit_slot_info = unit_slot_info,
    .unit_store = unit_store,
    .theme_bright = {127, 20, 20},  /* red */
    .theme_dim = {14, 2, 2},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
