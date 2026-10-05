/*
 * Korg Volca FM2: a DX7 voice engine. It takes whole voices only (DX7
 * single-voice SysEx dump), no single-parameter changes, and can't send its
 * voices back over MIDI - so the editor owns the voice, and every edit is
 * sent as a full dump, throttled: the Volca needs time per dump and drops
 * sounding notes when one arrives. Its own extras (chorus, reverb, arp...)
 * are Control Changes and not part of the voice.
 *
 * A patch file is the voice's 155 bytes of DX7 edit data (VCED), from
 * tools/dx7_import.py.
 *
 * Layout: Group A-F = OP1-OP6, parallel 3-page sections (OSC, EG, KEY
 * SCALING); G = the voice's common part (algorithm, pitch EG, LFO); H = the
 * Volca's own controls. Operators can be switched off like on the DX7 (not
 * part of the voice: a switched-off operator is sent with level 0).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "volca";

#define VCED_LEN 155
#define VCED_OP_LEN 21
#define VCED_COMMON 126  /* pitch EG onwards */
#define VCED_ALGORITHM 134
#define VCED_NAME 145

/* Dumps: after the last change has rested this long, or at most this often
 * while an encoder keeps turning */
#define DUMP_QUIET_US 120000
#define DUMP_MIN_INTERVAL_US 300000

/* ---- parameters ---- */

/* Per operator, in VCED order, plus the virtual on/off switch */
enum {
    F_R1, F_R2, F_R3, F_R4, F_L1, F_L2, F_L3, F_L4,
    F_BP, F_LD, F_RD, F_LC, F_RC, F_RS, F_AMS, F_KVS,
    F_OL, F_MODE, F_FC, F_FF, F_DET,
    F_ON,
    F_COUNT
};
#define OP_PARAM(op, f) ((op) * F_COUNT + (f))  /* op 0-5 = OP1-OP6 */

/* Common, in VCED order from VCED_COMMON */
enum {
    G_PR1 = 6 * F_COUNT, G_PR2, G_PR3, G_PR4, G_PL1, G_PL2, G_PL3, G_PL4,
    G_ALG, G_FB, G_OKS, G_LFS, G_LFD, G_LPMD, G_LAMD, G_LFKS, G_LFW, G_LPMS,
    G_TRNSP,
    /* The Volca's Control Changes */
    V_CHORUS, V_REVERB, V_VELOCITY, V_ARP_TYPE, V_ARP_DIV,
    P_COUNT
};
#define V_FIRST V_CHORUS

typedef enum {
    K_NUM,      /* 0..max as is */
    K_PLUSMINUS,/* value - center, signed */
    K_FREQ,     /* COARSE/FINE: the operator's resulting frequency */
    K_NOTE,     /* break point: 0 = A-1 */
    K_CURVE,    /* -LIN -EXP +EXP +LIN, drawn */
    K_WAVE,     /* LFO waveform, drawn */
    K_ALG,      /* 1-32 */
    K_SWITCH,
    K_EG,       /* envelope rate/level: value only, the envelope is drawn */
    K_PEG,      /* pitch EG level: signed, envelope drawn */
} kind_t;

typedef struct {
    const char *name;
    kind_t kind;
    uint8_t max, center;
} param_info_t;

static const param_info_t op_info[F_COUNT] = {
    [F_R1] = {"R1", K_EG, 99}, [F_R2] = {"R2", K_EG, 99},
    [F_R3] = {"R3", K_EG, 99}, [F_R4] = {"R4", K_EG, 99},
    [F_L1] = {"L1", K_EG, 99}, [F_L2] = {"L2", K_EG, 99},
    [F_L3] = {"L3", K_EG, 99}, [F_L4] = {"L4", K_EG, 99},
    [F_BP] = {"BREAK PT", K_NOTE, 99},
    [F_LD] = {"L DEPTH", K_NUM, 99}, [F_RD] = {"R DEPTH", K_NUM, 99},
    [F_LC] = {"L CURVE", K_CURVE, 3}, [F_RC] = {"R CURVE", K_CURVE, 3},
    [F_RS] = {"RATE SCALE", K_NUM, 7}, [F_AMS] = {"AM SENS", K_NUM, 3},
    [F_KVS] = {"VEL SENS", K_NUM, 7}, [F_OL] = {"LEVEL", K_NUM, 99},
    [F_MODE] = {"FIXED", K_SWITCH, 1}, [F_FC] = {"COARSE", K_FREQ, 31},
    [F_FF] = {"FINE", K_FREQ, 99}, [F_DET] = {"DETUNE", K_PLUSMINUS, 14, 7},
    [F_ON] = {"ON", K_SWITCH, 1},
};

static const param_info_t common_info[] = {
    {"R1", K_EG, 99}, {"R2", K_EG, 99}, {"R3", K_EG, 99}, {"R4", K_EG, 99},
    {"L1", K_PEG, 99, 50}, {"L2", K_PEG, 99, 50},
    {"L3", K_PEG, 99, 50}, {"L4", K_PEG, 99, 50},
    {"ALGORITHM", K_ALG, 31}, {"FEEDBACK", K_NUM, 7}, {"OSC SYNC", K_SWITCH, 1},
    {"SPEED", K_NUM, 99}, {"DELAY", K_NUM, 99}, {"PM DEPTH", K_NUM, 99},
    {"AM DEPTH", K_NUM, 99}, {"SYNC", K_SWITCH, 1}, {"WAVE", K_WAVE, 5},
    {"PM SENS", K_NUM, 7}, {"TRANSPOSE", K_PLUSMINUS, 48, 24},
    /* Volca CCs */
    {"CHORUS", K_NUM, 127}, {"REVERB", K_NUM, 127}, {"VELOCITY", K_NUM, 127},
    {"ARP TYPE", K_NUM, 127}, {"ARP DIV", K_NUM, 127},
};

static const uint8_t volca_cc[] = {93, 91, 41, 49, 50};  /* from V_FIRST */

static const char *const curve_names[] = {"-LIN", "-EXP", "+EXP", "+LIN"};
static const char *const wave_names[] = {"TRI", "SAW DN", "SAW UP", "SQUARE", "SINE", "S/H"};

/* ---- pages ---- */

#define _ SYNTH_NONE
#define OP_PAGES(op, n)                                                                  \
    {op, "OP" n " OSC",                                                                  \
     {OP_PARAM(op, F_FC), OP_PARAM(op, F_FF), OP_PARAM(op, F_DET), OP_PARAM(op, F_OL),  \
      OP_PARAM(op, F_KVS), OP_PARAM(op, F_AMS), _, _},                                   \
     {OP_PARAM(op, F_MODE), _, _, OP_PARAM(op, F_ON), _, _, _, _}},                      \
    {op, "OP" n " EG",                                                                   \
     {OP_PARAM(op, F_R1), OP_PARAM(op, F_L1), OP_PARAM(op, F_R2), OP_PARAM(op, F_L2),   \
      OP_PARAM(op, F_R3), OP_PARAM(op, F_L3), OP_PARAM(op, F_R4), OP_PARAM(op, F_L4)},  \
     {_, _, _, _, _, _, _, _}},                                                          \
    {op, "OP" n " KEY SCALING",                                                          \
     {OP_PARAM(op, F_BP), OP_PARAM(op, F_LC), OP_PARAM(op, F_LD), OP_PARAM(op, F_RD),   \
      OP_PARAM(op, F_RC), OP_PARAM(op, F_RS), _, _},                                     \
     {_, _, _, _, _, _, _, _}}

#define PAGE_ALGORITHM 18
#define PAGE_PITCH_EG 19
static const synth_page_t pages[] = {
    OP_PAGES(0, "1"), OP_PAGES(1, "2"), OP_PAGES(2, "3"),
    OP_PAGES(3, "4"), OP_PAGES(4, "5"), OP_PAGES(5, "6"),
    {6, "ALGORITHM", {G_ALG, G_FB, G_TRNSP, _, _, _, _, _}, {_, _, _, G_OKS, _, _, _, _}},
    {6, "PITCH EG", {G_PR1, G_PL1, G_PR2, G_PL2, G_PR3, G_PL3, G_PR4, G_PL4},
     {_, _, _, _, _, _, _, _}},
    {6, "LFO", {G_LFW, G_LFS, G_LFD, G_LPMD, G_LAMD, G_LPMS, _, _},
     {G_LFKS, _, _, _, _, _, _, _}},
    {7, "VOLCA", {V_CHORUS, V_REVERB, V_VELOCITY, V_ARP_TYPE, V_ARP_DIV, _, _, _},
     {_, _, _, _, _, _, _, _}},
};
#undef _

static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    [SETTING_PAD_NOTE] = {"PAD LOW", SET_NOTE, 0, 112, 48},
};

/* ---- the DX7's 32 algorithms ---- */

/* mods[op] = bit mask of the operators modulating op (bit 0 = OP1);
 * carriers = mask; feedback: from op's output into to op */
typedef struct {
    uint8_t mods[6];
    uint8_t carriers;
    uint8_t fb_from, fb_to;  /* 1-6 */
} algorithm_t;

#define B(n) (1 << ((n) - 1))
static const algorithm_t algorithms[32] = {
    /*  1 */ {{B(2), 0, B(4), B(5), B(6), 0}, B(1) | B(3), 6, 6},
    /*  2 */ {{B(2), 0, B(4), B(5), B(6), 0}, B(1) | B(3), 2, 2},
    /*  3 */ {{B(2), B(3), 0, B(5), B(6), 0}, B(1) | B(4), 6, 6},
    /*  4 */ {{B(2), B(3), 0, B(5), B(6), 0}, B(1) | B(4), 4, 6},
    /*  5 */ {{B(2), 0, B(4), 0, B(6), 0}, B(1) | B(3) | B(5), 6, 6},
    /*  6 */ {{B(2), 0, B(4), 0, B(6), 0}, B(1) | B(3) | B(5), 5, 6},
    /*  7 */ {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 6, 6},
    /*  8 */ {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 4, 4},
    /*  9 */ {{B(2), 0, B(4) | B(5), 0, B(6), 0}, B(1) | B(3), 2, 2},
    /* 10 */ {{B(2), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(4), 3, 3},
    /* 11 */ {{B(2), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(4), 6, 6},
    /* 12 */ {{B(2), 0, B(4) | B(5) | B(6), 0, 0, 0}, B(1) | B(3), 2, 2},
    /* 13 */ {{B(2), 0, B(4) | B(5) | B(6), 0, 0, 0}, B(1) | B(3), 6, 6},
    /* 14 */ {{B(2), 0, B(4), B(5) | B(6), 0, 0}, B(1) | B(3), 6, 6},
    /* 15 */ {{B(2), 0, B(4), B(5) | B(6), 0, 0}, B(1) | B(3), 2, 2},
    /* 16 */ {{B(2) | B(3) | B(5), 0, B(4), 0, B(6), 0}, B(1), 6, 6},
    /* 17 */ {{B(2) | B(3) | B(5), 0, B(4), 0, B(6), 0}, B(1), 2, 2},
    /* 18 */ {{B(2) | B(3) | B(4), 0, 0, B(5), B(6), 0}, B(1), 3, 3},
    /* 19 */ {{B(2), B(3), 0, B(6), B(6), 0}, B(1) | B(4) | B(5), 6, 6},
    /* 20 */ {{B(3), B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 3, 3},
    /* 21 */ {{B(3), B(3), 0, B(6), B(6), 0}, B(1) | B(2) | B(4) | B(5), 3, 3},
    /* 22 */ {{B(2), 0, B(6), B(6), B(6), 0}, B(1) | B(3) | B(4) | B(5), 6, 6},
    /* 23 */ {{0, B(3), 0, B(6), B(6), 0}, B(1) | B(2) | B(4) | B(5), 6, 6},
    /* 24 */ {{0, 0, B(6), B(6), B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 6, 6},
    /* 25 */ {{0, 0, 0, B(6), B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 6, 6},
    /* 26 */ {{0, B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 6, 6},
    /* 27 */ {{0, B(3), 0, B(5) | B(6), 0, 0}, B(1) | B(2) | B(4), 3, 3},
    /* 28 */ {{B(2), 0, B(4), B(5), 0, 0}, B(1) | B(3) | B(6), 5, 5},
    /* 29 */ {{0, 0, B(4), 0, B(6), 0}, B(1) | B(2) | B(3) | B(5), 6, 6},
    /* 30 */ {{0, 0, B(4), B(5), 0, 0}, B(1) | B(2) | B(3) | B(6), 5, 5},
    /* 31 */ {{0, 0, 0, 0, B(6), 0}, B(1) | B(2) | B(3) | B(4) | B(5), 6, 6},
    /* 32 */ {{0, 0, 0, 0, 0, 0}, 0x3F, 6, 6},
};
#undef B

/* ---- state ---- */

static uint8_t voice[VCED_LEN], init_voice[VCED_LEN];
static bool op_on[6] = {true, true, true, true, true, true};
static uint8_t cc_values[P_COUNT - V_FIRST];
static bool cc_known[P_COUNT - V_FIRST];
static bool init_read, dirty;
static int64_t changed_at, sent_at;

static const param_info_t *info(int p)
{
    return p < G_PR1 ? &op_info[p % F_COUNT] : &common_info[p - G_PR1];
}

/* Where p lives in the voice, or -1 (switch / Volca CC) */
static int offset(int p)
{
    if (p < G_PR1) {
        int op = p / F_COUNT, f = p % F_COUNT;
        return f == F_ON ? -1 : (5 - op) * VCED_OP_LEN + f;  /* stored OP6 first */
    }
    return p < V_FIRST ? VCED_COMMON + (p - G_PR1) : -1;
}

static int value(int p)
{
    if (p >= V_FIRST) {
        return cc_values[p - V_FIRST];
    }
    if (p < G_PR1 && p % F_COUNT == F_ON) {
        return op_on[p / F_COUNT];
    }
    return voice[offset(p)];
}

static int op_value(int op, int f) { return voice[(5 - op) * VCED_OP_LEN + f]; }

/* The envelope (0-5 = OP1-OP6, 6 = pitch EG) as rates[4], levels[4] */
static void envelope_values(int env, uint8_t *r, uint8_t *l)
{
    for (int i = 0; i < 4; i++) {
        r[i] = env < 6 ? op_value(env, F_R1 + i) : voice[VCED_COMMON + i];
        l[i] = env < 6 ? op_value(env, F_L1 + i) : voice[VCED_COMMON + 4 + i];
    }
}

/* While an envelope is being changed, its earlier shape stays drawn dotted
 * for a moment: the panel's own afterglow (slow to turn pixels off) only
 * lingers on parts of the old line, this makes the whole old line linger
 * evenly instead */
#define TRAIL_REFRESH_US 350000  /* the trail follows at most this often */
#define TRAIL_HOLD_US 150000     /* and stays this long after the last change */
static uint8_t trail_r[4], trail_l[4];
static int trail_env = -1;  /* 0-5 = OP1-OP6 EG, 6 = pitch EG */
static int64_t trail_taken, trail_until;

static void send_voice(void)
{
    uint8_t msg[6 + VCED_LEN + 2] = {0xF0, 0x43, 0x00 | synth_setting(SETTING_CHANNEL), 0x00, 0x01, 0x1B};
    uint8_t sum = 0;
    for (int i = 0; i < VCED_LEN; i++) {
        uint8_t v = voice[i] & 0x7F;
        if (i < 6 * VCED_OP_LEN && i % VCED_OP_LEN == F_OL && !op_on[5 - i / VCED_OP_LEN]) {
            v = 0;  /* switched off */
        }
        msg[6 + i] = v;
        sum += v;
    }
    msg[6 + VCED_LEN] = (uint8_t)(-sum) & 0x7F;
    msg[7 + VCED_LEN] = 0xF7;
    if (midi_send_sysex(msg, sizeof(msg))) {
        dirty = false;
        sent_at = esp_timer_get_time();
    }
}

static void changed(void)
{
    dirty = true;
    changed_at = esp_timer_get_time();
}

static void set_value(int p, int v)
{
    if (p >= V_FIRST) {
        cc_values[p - V_FIRST] = v;
        cc_known[p - V_FIRST] = true;
        midi_send(0xB0 | synth_setting(SETTING_CHANNEL), volca_cc[p - V_FIRST], v);
        return;
    }
    kind_t k = info(p)->kind;
    if (k == K_EG || k == K_PEG) {
        int env = p < G_PR1 ? p / F_COUNT : 6;
        int64_t now = esp_timer_get_time();
        if (env != trail_env || now - trail_taken > TRAIL_REFRESH_US) {
            envelope_values(env, trail_r, trail_l);  /* the shape before this change */
            trail_env = env;
            trail_taken = now;
        }
        trail_until = now + TRAIL_HOLD_US;
    }
    if (offset(p) < 0) {
        op_on[p / F_COUNT] = v;
    } else {
        voice[offset(p)] = v;
    }
    changed();
}

static void tick(int64_t now)
{
    if (trail_env >= 0 && now > trail_until) {
        trail_env = -1;
        synth_redraw_page();
    }
    if (dirty && (now - changed_at > DUMP_QUIET_US || now - sent_at > DUMP_MIN_INTERVAL_US)) {
        send_voice();
    }
}

static bool read_voice(const char *path, uint8_t *dst)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(dst, 1, VCED_LEN, f);
    fclose(f);
    return n == VCED_LEN;
}

static void read_init(void)
{
    if (!init_read) {
        init_read = read_voice(STORAGE_ROOT "/dx7/000 Init", init_voice);
    }
}

/* ---- formatting ---- */

static void format_freq(int op, char *buf, size_t len)
{
    int fc = op_value(op, F_FC), ff = op_value(op, F_FF);
    if (op_value(op, F_MODE)) {
        float hz = powf(10.0f, (fc & 3) + ff / 100.0f);
        if (hz < 10.0f) {
            snprintf(buf, len, "%.2fHz", hz);
        } else if (hz < 100.0f) {
            snprintf(buf, len, "%.1fHz", hz);
        } else if (hz < 1000.0f) {
            snprintf(buf, len, "%.0fHz", hz);
        } else {
            snprintf(buf, len, "%.2fk", hz / 1000.0f);
        }
    } else {
        float r = (fc ? fc : 0.5f) * (1.0f + ff / 100.0f);
        snprintf(buf, len, r < 10.0f ? "x%.2f" : "x%.1f", r);
    }
}

static void format_signed(int v, char *buf, size_t len)
{
    snprintf(buf, len, v ? "%+d" : "0", v);
}

static const char *param_name(int p) { return info(p)->name; }

static void param_format(int p, char *buf, size_t len)
{
    const param_info_t *in = info(p);
    if (p >= V_FIRST && !cc_known[p - V_FIRST]) {
        snprintf(buf, len, "--");
        return;
    }
    int v = value(p);
    switch (in->kind) {
    case K_PLUSMINUS:
    case K_PEG:
        format_signed(v - in->center, buf, len);
        break;
    case K_FREQ:
        format_freq(p / F_COUNT, buf, len);
        break;
    case K_NOTE: {
        static const char *const names[] = {"C", "C#", "D", "D#", "E", "F",
                                            "F#", "G", "G#", "A", "A#", "B"};
        int n = v + 9;  /* 0 = A-1 */
        snprintf(buf, len, "%s%d", names[n % 12], n / 12 - 1);
        break;
    }
    case K_CURVE:
        snprintf(buf, len, "%s", curve_names[v & 3]);
        break;
    case K_WAVE:
        snprintf(buf, len, "%s", wave_names[v <= 5 ? v : 0]);
        break;
    case K_ALG:
        snprintf(buf, len, "%d", v + 1);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    const param_info_t *in = info(p);
    if (in->kind == K_EG || in->kind == K_PEG || (p >= V_FIRST && !cc_known[p - V_FIRST])) {
        return -1;
    }
    return value(p) * 1000 / in->max;
}

static bool param_on(int p) { return value(p) != 0; }

static int param_units(int p)
{
    int max = info(p)->max;
    return max <= 3 ? 80 : max <= 7 ? 60 : max <= 31 ? 30 : max <= 48 ? 20 : 10;
}

static bool param_step(int p, int steps)
{
    int v = value(p) + steps, max = info(p)->max;
    v = v < 0 ? 0 : v > max ? max : v;
    if (p >= V_FIRST ? cc_known[p - V_FIRST] && v == value(p) : v == value(p)) {
        return false;
    }
    set_value(p, v);
    return true;
}

static void param_toggle(int p) { set_value(p, !value(p)); }

static bool param_reset(int p)
{
    if (p >= V_FIRST) {
        return false;  /* not part of the voice */
    }
    int v;
    if (offset(p) < 0) {
        v = 1;  /* operator on */
    } else {
        read_init();
        v = init_voice[offset(p)];
    }
    if (v == value(p)) {
        return false;
    }
    set_value(p, v);
    return true;
}

/* ---- pictures ---- */

/* Envelopes span every column: a band at the bottom of both displays */
#define BAND_Y 14
#define BAND_H 38
#define EG_TEXT_CAP_Y 55  /* name and value under the envelope */
#define SLOT_W 128  /* one rate/level column pair per envelope stage */

/* Global x 0-511 over both displays (a segment never spans both); dotted =
 * every other pixel */
static void band_line(int gx0, int y0, int gx1, int y1, bool dotted)
{
    int disp = gx0 / 256, x0 = gx0 % 256, x1 = gx1 - disp * 256;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int n = 0;; n++) {
        if (!dotted || !(n & 1)) {
            display_pixel(disp, x0, y0, true);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}


/* An envelope from L4 through L1, L2, L3 (sustain) and back to L4 (release),
 * one 128 px stage per rate/level column pair; a stage's ramp is as wide as
 * its rate is slow (rates are already logarithmic in time) */
static void draw_stages(const uint8_t *rates, const uint8_t *levels, int disp, bool dotted)
{
    int prev = levels[3];
    for (int i = 0; i < 4; i++) {
        if (i / 2 != disp) {
            prev = levels[i];
            continue;
        }
        int x0 = i * SLOT_W, x_end = x0 + SLOT_W - (i == 3 ? 2 : 1);
        int ramp = 2 + (SLOT_W - 6) * (99 - rates[i]) / 99;
        int y0 = BAND_Y + BAND_H - 1 - prev * (BAND_H - 1) / 99;
        int y1 = BAND_Y + BAND_H - 1 - levels[i] * (BAND_H - 1) / 99;
        band_line(x0, y0, x0 + ramp, y1, dotted);
        band_line(x0 + ramp, y1, x_end, y1, dotted);
        prev = levels[i];
    }
}

static void draw_envelope(const uint8_t *rates, const uint8_t *levels, bool centred, int disp,
                          int env)
{
    display_fill_rect(disp, 0, BAND_Y, 256, BAND_H, false);
    if (centred) {
        for (int x = 0; x < 255; x += 4) {
            display_pixel(disp, x, BAND_Y + BAND_H / 2, true);
        }
    }
    if (env == trail_env) {
        draw_stages(trail_r, trail_l, disp, true);
    }
    draw_stages(rates, levels, disp, false);
}


/* A key scaling curve's shape: level change going away from the break
 * point (right edge for the left curve, left edge for the right one) */
static void draw_curve(int disp, int x, int y, int w, int h, int curve, bool left)
{
    int mid = y + h / 2;
    for (int i = 0; i < w; i += 3) {
        display_pixel(disp, x + i, mid, true);
    }
    int px = 0, py = mid;
    for (int i = 0; i <= w; i++) {
        float d = (float)i / w;  /* distance from the break point */
        float a = (curve == 0 || curve == 3) ? d : d * d * d;
        int yy = mid + (int)((curve >= 2 ? -1 : 1) * a * (h / 2));
        int xx = left ? x + w - i : x + i;
        if (i) {
            display_line(disp, px, py, xx, yy, true);
        }
        px = xx;
        py = yy;
    }
}

static void draw_lfo_wave(int disp, int x, int y, int w, int h, int wave)
{
    static const uint8_t sh[8] = {3, 9, 1, 12, 6, 14, 0, 8};  /* sample & hold steps */
    int px = 0, py = 0;
    for (int i = 0; i <= w; i++) {
        float t = 2.0f * i / w, ph = t - floorf(t);  /* two cycles */
        float v;  /* 0 = bottom, 1 = top */
        switch (wave) {
        case 0: v = ph < 0.5f ? ph * 2 : 2 - ph * 2; break;
        case 1: v = 1 - ph; break;
        case 2: v = ph; break;
        case 3: v = ph < 0.5f ? 1 : 0; break;
        case 4: v = 0.5f + 0.5f * sinf(2 * (float)M_PI * ph); break;
        default: v = sh[i * 8 / (w + 1)] / 14.0f; break;
        }
        int xx = x + i, yy = y + h - 1 - (int)(v * (h - 1) + 0.5f);
        if (i) {
            display_line(disp, px, py, xx, yy, true);
        }
        px = xx;
        py = yy;
    }
}

/* Envelope columns: just "R1 99" at the bottom, the envelope (page_draw)
 * gets the rest */
static bool column_draw(int p, int disp, int x)
{
    const param_info_t *in = info(p);
    if (in->kind != K_EG && in->kind != K_PEG) {
        return false;
    }
    char text[16], val[8];
    param_format(p, val, sizeof(val));
    snprintf(text, sizeof(text), "%s %s", in->name, val);
    int tw = display_text_width(&font_small, text);
    display_text(disp, x + (63 - tw) / 2, EG_TEXT_CAP_Y - font_small.cap_top, &font_small, text, true);
    return true;
}

static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    const param_info_t *in = info(p);
    if (in->kind == K_CURVE) {
        draw_curve(disp, x + 9, y, w - 18, font_large.cap_height, value(p) & 3,
                   p % F_COUNT == F_LC);
        return SYNTH_DRAW_LABEL;
    }
    if (in->kind == K_WAVE) {
        draw_lfo_wave(disp, x + 9, y, w - 18, font_large.cap_height, value(p));
        return SYNTH_DRAW_LABEL;
    }
    return SYNTH_DRAW_NONE;
}

/* The algorithm as on the DX7's panel: carriers at the bottom joined to the
 * output, modulators stacked above what they modulate, the feedback loop;
 * switched-off operators drawn hollow */
#define ALG_BOX_W 15
#define ALG_BOX_H 9
#define ALG_ROW_H 12
#define ALG_BUS_Y 63  /* 4 rows + the feedback loop fit under the header */

static int alg_depth(const algorithm_t *a, int op)
{
    if (a->carriers & (1 << op)) {
        return 0;
    }
    int d = 0;
    for (int t = 0; t < 6; t++) {
        if (a->mods[t] & (1 << op)) {
            int dt = alg_depth(a, t) + 1;
            d = dt > d ? dt : d;
        }
    }
    return d;
}

static float alg_place(const algorithm_t *a, int op, float *x, bool *placed, int *cursor)
{
    placed[op] = true;
    float sum = 0;
    int n = 0;
    for (int m = 0; m < 6; m++) {
        if ((a->mods[op] & (1 << m)) && !placed[m]) {
            sum += alg_place(a, m, x, placed, cursor);
            n++;
        }
    }
    x[op] = n ? sum / n : (*cursor)++;
    return x[op];
}

static void draw_algorithm(int disp)
{
    const algorithm_t *a = &algorithms[voice[VCED_ALGORITHM] & 31];
    display_fill_rect(disp, 0, 12, 256, 52, false);
    float slot[6];
    bool placed[6] = {false};
    int cursor = 0;
    for (int c = 0; c < 6; c++) {
        if (a->carriers & (1 << c)) {
            alg_place(a, c, slot, placed, &cursor);
        }
    }
    int spacing = cursor > 1 ? (240 / cursor < 34 ? 240 / cursor : 34) : 34;
    int left = (255 - (cursor - 1) * spacing) / 2;
    int cx[6], top[6];
    for (int op = 0; op < 6; op++) {
        cx[op] = left + (int)(slot[op] * spacing + 0.5f);
        top[op] = ALG_BUS_Y - 3 - ALG_BOX_H - alg_depth(a, op) * ALG_ROW_H;
    }
    /* output bus */
    int bus_l = 255, bus_r = 0;
    for (int op = 0; op < 6; op++) {
        if (a->carriers & (1 << op)) {
            display_line(disp, cx[op], top[op] + ALG_BOX_H, cx[op], ALG_BUS_Y, true);
            bus_l = cx[op] < bus_l ? cx[op] : bus_l;
            bus_r = cx[op] > bus_r ? cx[op] : bus_r;
        }
    }
    display_line(disp, bus_l, ALG_BUS_Y, bus_r, ALG_BUS_Y, true);
    /* modulation */
    for (int t = 0; t < 6; t++) {
        for (int m = 0; m < 6; m++) {
            if (a->mods[t] & (1 << m)) {
                display_line(disp, cx[m], top[m] + ALG_BOX_H, cx[t], top[t] - 1, true);
            }
        }
    }
    /* feedback: out of fb_from's right side, up and into fb_to's top */
    int f = a->fb_from - 1, t = a->fb_to - 1;
    int fx = cx[f] + ALG_BOX_W / 2 + 4, ty = top[t] - 3;
    display_line(disp, cx[f] + ALG_BOX_W / 2, top[f] + ALG_BOX_H - 2, fx, top[f] + ALG_BOX_H - 2, true);
    display_line(disp, fx, top[f] + ALG_BOX_H - 2, fx, ty, true);
    display_line(disp, fx, ty, cx[t], ty, true);
    display_line(disp, cx[t], ty, cx[t], top[t] - 1, true);
    /* operators */
    for (int op = 0; op < 6; op++) {
        int bx = cx[op] - ALG_BOX_W / 2;
        char num[2] = {(char)('1' + op), 0};
        display_fill_rect(disp, bx, top[op], ALG_BOX_W, ALG_BOX_H, false);
        display_round_rect(disp, bx, top[op], ALG_BOX_W, ALG_BOX_H, op_on[op], true);
        display_text(disp, bx + (ALG_BOX_W - display_text_width(&font_tiny, num)) / 2 + 1,
                     top[op] + 2 - font_tiny.cap_top, &font_tiny, num, !op_on[op]);
    }
}

static void page_draw(int pg, int disp)
{
    if (pg < PAGE_ALGORITHM && pg % 3 == 1) {
        uint8_t r[4], l[4];
        envelope_values(pg / 3, r, l);
        draw_envelope(r, l, false, disp, pg / 3);
    } else if (pg == PAGE_PITCH_EG) {
        uint8_t r[4], l[4];
        envelope_values(6, r, l);
        draw_envelope(r, l, true, disp, 6);
    } else if (pg == PAGE_ALGORITHM && disp == 1) {
        draw_algorithm(disp);
    }
}

/* Group buttons: carriers green, modulators blue, switched-off operators
 * nearly dark */
static bool section_dim(int section, uint8_t rgb[3])
{
    if (section > 5) {
        return false;
    }
    static const uint8_t carrier[3] = {0, 12, 4}, modulator[3] = {0, 5, 14}, off[3] = {2, 2, 2};
    const uint8_t *c = !op_on[section] ? off
                       : algorithms[voice[VCED_ALGORITHM] & 31].carriers & (1 << section) ? carrier
                       : modulator;
    memcpy(rgb, c, 3);
    return true;
}

/* ---- patches ---- */

static bool patch_load(const char *path)
{
    if (!read_voice(path, voice)) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    for (int op = 0; op < 6; op++) {
        op_on[op] = true;
    }
    send_voice();
    return true;
}

static bool patch_save(const char *path)
{
    /* The voice's own name (10 characters) follows the file's */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    for (int i = 0; i < 10; i++) {
        voice[VCED_NAME + i] = *title ? *title++ : ' ';
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(voice, 1, VCED_LEN, f) == VCED_LEN;
    fclose(f);
    return ok;
}

static int unknown_count(void) { return 0; }

const synth_t synth_volca = {
    .name = "Volca FM2",
    .key = "volca",
    .patch_dir = STORAGE_ROOT "/dx7",
    .default_patch = "000 Init",
    .first_user_patch = 300,
    .pages = pages,
    .page_count = sizeof(pages) / sizeof(pages[0]),
    .settings = settings,
    .setting_count = sizeof(settings) / sizeof(settings[0]),
    .param_name = param_name,
    .param_format = param_format,
    .param_bar = param_bar,
    .param_on = param_on,
    .param_units = param_units,
    .param_step = param_step,
    .param_toggle = param_toggle,
    .param_draw = param_draw,
    .param_reset = param_reset,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .unknown_count = unknown_count,
    .tick = tick,
    .page_draw = page_draw,
    .section_dim = section_dim,
    .column_draw = column_draw,
    .theme_bright = {0, 127, 40},  /* green */
    .theme_dim = {0, 12, 4},
};
