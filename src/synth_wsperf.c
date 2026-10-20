/*
 * Korg Wavestation SR, PERF: a performance is a name, the effects block and eight parts, each part playing a
 * patch of the SR's own memory (bank + number) with its level, FX bus, key and velocity range, tuning and
 * play settings. The patches are edited in the PATCH entry; here they are put together.
 *
 * Layout: A PART (page 1: patch, level, bus, key and velocity range; page 2: tuning, delay, play settings).
 * The part is picked with Shift + Group A-H or the transport Step arrows. The effects block (21 bytes: types,
 * routing, mixes and the effects' own parameters, packed by effect group) is kept as it is and sent back as it
 * came - not edited yet.
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

enum {
    P_BANK, P_PATCH, P_VOLUME, P_FXBUS, P_LOKEY, P_HIKEY, P_LOVEL, P_HIVEL, P_TRANS, P_DETUNE, P_DELAY, P_SCALE,
    P_SCALEKEY, P_PLAY, P_VOICE, P_PRIO, P_SUSTAIN, P_COUNT
};

typedef struct {
    const char *name;
    int16_t min, max, def;
} pinfo_t;

static const pinfo_t pinfo[P_COUNT] = {
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
};

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
enum { PG_PART, PG_PART2 };
static const synth_page_t pages[] = {
    [PG_PART] = {0, "PART", {P_BANK, P_PATCH, P_VOLUME, P_FXBUS, P_LOKEY, P_HIKEY, P_LOVEL, P_HIVEL}, NO_SW},
    [PG_PART2] = {0, "PART 2", {P_TRANS, P_DETUNE, P_DELAY, P_SCALE, P_SCALEKEY, P_PLAY, P_VOICE, P_PRIO},
                  {P_SUSTAIN, _, _, _, _, _, _, _}},
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

static bool param_step(int p, int steps)
{
    ensure();
    int v = get(p, part), nv = clampi(v + steps, pinfo[p].min, pinfo[p].max);
    if (nv == v) {
        return false;
    }
    set(p, part, nv);
    changed();
    return true;
}

static void param_toggle(int p)
{
    ensure();
    set(p, part, !get(p, part));
    changed();
}

static bool param_reset(int p)
{
    ensure();
    if (get(p, part) == pinfo[p].def) {
        return false;
    }
    set(p, part, pinfo[p].def);
    changed();
    return true;
}

static const char *param_name(int p) { return pinfo[p].name; }

static void param_format(int p, char *buf, size_t len)
{
    ensure();
    int v = get(p, part);
    switch (p) {
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
    if (p == P_BANK || p == P_PATCH || p == P_SCALE || p == P_PLAY || p == P_VOICE || p == P_PRIO || p == P_SUSTAIN) {
        return -1;
    }
    return (get(p, part) - pinfo[p].min) * 1000 / (pinfo[p].max - pinfo[p].min);
}

static bool param_on(int p)
{
    ensure();
    return p == P_SUSTAIN && get(P_SUSTAIN, part);
}

static int param_units(int p)
{
    int range = pinfo[p].max - pinfo[p].min;
    return range <= 1 ? 80 : range <= 6 ? 70 : range <= 16 ? 40 : range <= 30 ? 25 : 10;
}

/* The part's patch: its place big, its name below */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
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

/* ---- overview (Grid): the eight parts as a table (no header: eight rows fill the display) ---- */

static void overview_draw(int page, int disp)
{
    (void)page;
    ensure();
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
    midi_send_sysex(msg, sizeof(msg));
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
                     "key and velocity range; page 2: tuning,", "delay, play settings. Effects: not edited",
                     "yet, kept as they are."},
     {BTN_GROUP_A, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT, -1, -1, -1, -1}},
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
    .overview_pages = 1,
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
