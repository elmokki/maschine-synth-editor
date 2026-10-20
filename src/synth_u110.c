/*
 * Roland U-110 (PCM sound module): a patch is a setup of six parts, each
 * playing one PCM tone (the internal ROM or an SN-U110 card) on its own
 * channel, with a few settings per part (level, key range, envelope rates,
 * pitch, an LFO, pressure sensitivities) and common ones (output mode,
 * chorus, tremolo). There is no filter: the tone is the sample.
 *
 * Layout: Group A PART (tone, level, output, channel, key range, bend; page 2
 * the program change), B ENV / PITCH (also the sensitivities), C LFO,
 * D COMMON. The part is picked with Shift + Group A-F or the transport Step
 * arrows.
 *
 * Every change goes out as an individual-parameter DT1 (manual p. 90: one
 * parameter a message, at least ~20 ms apart; needs SETUP: MIDI EXCLUSIVE ON
 * on the unit), from tick() so fast knob turns are coalesced. Selecting a
 * library patch sends all of it, which takes a few seconds. The unit's
 * device ID is its control channel (the CONTROL CH setting).
 *
 * Not done yet: reading the unit's patches back and writing into its own
 * memory (bulk dumps are nibble-packed; Edisyn's RolandU110.java has the
 * layout, which the manual doesn't). The SysEx map has no rhythm setup, so
 * "Drums" is just a tone.
 *
 * A patch file is 171 bytes: the name (10), the 5 common parameters and 6 x
 * 26 part parameters, each in the order of its address (00 1n 00 .. 00 1n 19).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "esp_log.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "u110";

#define MODEL_ID 0x23
#define ADDR(a, b, c) ((uint32_t)(a) << 14 | (uint32_t)(b) << 7 | (uint32_t)(c))

#define NAME_LEN 10
#define PARTS 6
#define COMMON_N 5
#define PART_N 26
#define COMMON_AT NAME_LEN
#define PART_AT (NAME_LEN + COMMON_N)
#define PATCH_LEN (PART_AT + PARTS * PART_N)
#define TONES 99
#define TONE_NAME_LEN 16
#define SEND_GAP_US 25000
#define MODE_PAUSE_US 400000  /* after an output mode change */

/* ---- parameters ---- */

typedef enum {
    K_NUM,
    K_SIGNED,   /* value - center */
    K_PLUS1,    /* stored 0.., shown 1.. */
    K_SWITCH,
    K_NOTE,     /* key range: note name */
    K_OUT,      /* output assign: 1-6, 6 = OFF */
    K_POLY,     /* pitch poly press sens: semitones from a table */
    K_MEDIA,    /* INT, C01-C31 */
    K_TONE,
    K_OUTMODE,
} kind_t;

typedef struct {
    const char *name;  /* fits a 64 px display column */
    kind_t kind;
    int16_t min, max, center;
} u_param_t;

/* Common parameters, then a part's in the order of their addresses */
enum {
    P_OUTMODE, P_CHO_RATE, P_CHO_DEPTH, P_TREM_RATE, P_TREM_DEPTH,
    P_OUT, P_RX_CH, P_MEDIA, P_TONE, P_BEND, P_KEY_LO, P_KEY_HI, P_LEVEL, P_VEL, P_LVL_PRESS, P_ATTACK,
    P_RELEASE, P_COARSE, P_FINE, P_LFO_RATE, P_LFO_DELAY, P_LFO_AUTO_RISE, P_LFO_AUTO_DEPTH, P_LFO_MAN_RISE,
    P_LFO_MAN_DEPTH, P_LFO_CH_PRESS, P_PGM_CHG, P_PGM_MAP, P_DETUNE, P_PITCH_PRESS, P_LFO_POLY,
    PARAM_COUNT
};
_Static_assert(PARAM_COUNT == COMMON_N + PART_N, "part parameters follow the address map");

#define NUM(name, max) {name, K_NUM, 0, max, 0}

static const u_param_t params[PARAM_COUNT] = {
    [P_OUTMODE] = {"OUT MODE", K_OUTMODE, 0, 49, 0},
    [P_CHO_RATE] = NUM("CHO RATE", 15),
    [P_CHO_DEPTH] = NUM("CHO DEPTH", 15),
    [P_TREM_RATE] = NUM("TRM RATE", 15),
    [P_TREM_DEPTH] = NUM("TRM DEPTH", 15),

    [P_OUT] = {"OUTPUT", K_OUT, 0, 6, 0},
    [P_RX_CH] = {"RX CH", K_PLUS1, 0, 15, 0},
    [P_MEDIA] = {"MEDIA", K_MEDIA, 0, 31, 0},
    [P_TONE] = {"TONE", K_TONE, 0, TONES - 1, 0},
    [P_BEND] = NUM("BEND", 12),
    [P_KEY_LO] = {"KEY LO", K_NOTE, 0, 127, 0},
    [P_KEY_HI] = {"KEY HI", K_NOTE, 0, 127, 0},
    [P_LEVEL] = NUM("LEVEL", 127),
    [P_VEL] = NUM("VEL SENS", 15),
    [P_LVL_PRESS] = NUM("LVL PRESS", 15),
    [P_ATTACK] = {"ATTACK", K_SIGNED, 1, 15, 8},
    [P_RELEASE] = {"RELEASE", K_SIGNED, 1, 15, 8},
    [P_COARSE] = {"COARSE", K_SIGNED, 52, 76, 64},
    [P_FINE] = {"FINE", K_SIGNED, 14, 114, 64},
    [P_LFO_RATE] = NUM("LFO RATE", 15),
    [P_LFO_DELAY] = NUM("A.DELAY", 15),
    [P_LFO_AUTO_RISE] = NUM("A.RISE", 15),
    [P_LFO_AUTO_DEPTH] = NUM("A.DEPTH", 15),
    [P_LFO_MAN_RISE] = NUM("M.RISE", 15),
    [P_LFO_MAN_DEPTH] = NUM("M.DEPTH", 15),
    [P_LFO_CH_PRESS] = NUM("LFO CH PR", 7),
    [P_PGM_CHG] = {"PGM CHG", K_SWITCH, 0, 1, 0},
    [P_PGM_MAP] = {"PGM MAP", K_PLUS1, 0, 5, 0},
    [P_DETUNE] = NUM("DETUNE", 15),
    [P_PITCH_PRESS] = {"PITCH PR", K_POLY, 0, 15, 0},
    [P_LFO_POLY] = NUM("LFO POLY", 7),
};

static const int8_t poly_semitones[16] = {-24, -12, -7, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 7, 12};

static const char *const media_names[16] = {
    "Internal", "Organ / Harpsi", "Latin / FX Perc", "Ethnic", "E.Grand / Clavi", "Orch Strings", "Orch Winds",
    "Electric Guitar", "Synthesizer", "Guitar / Keys", "Rock Drums", "Sound Effects", "Sax / Trombone",
    "Super Strings", "Super Ac Guitar", "Super Brass",
};

/* What the output modes do with the 31 voices: output number : voices
 * (S / M = outputs 1-2 as stereo / mono) */
static const char *const outmode_text[50] = {
    "1:31", "1:27 2:4", "1:23 2:8", "1:23 2:4 3:4", "1:19 2:12", "1:19 2:8 3:4", "1:19 2:4 3:4 4:4",
    "1:15 2:16", "1:15 2:12 3:4", "1:15 2:8 3:8", "1:15 2:8 3:4 4:4", "1:15 2:4 3:4 4:4 5:4",
    "1:11 2:12 3:8", "1:11 2:12 3:4 4:4", "1:11 2:8 3:8 4:4", "1:11 2:8 3:4 4:4 5:4",
    "1:11 2:4 3:4 4:4 5:4 6:4", "1:7 2:8 3:8 4:8", "1:7 2:8 3:8 4:4 5:4", "1:7 2:8 3:4 4:4 5:4 6:4",
    "1-2:S31", "1-2:M31", "1-2:S16 3:15", "1-2:M16 3:15", "1-2:S16 3:11 4:4", "1-2:M16 3:11 4:4",
    "1-2:S16 3:7 4:8", "1-2:M16 3:7 4:8", "1-2:S16 3:7 4:4 5:4", "1-2:M16 3:7 4:4 5:4",
    "1-2:S16 3:3 4:4 5:4 6:4", "1-2:M16 3:3 4:4 5:4 6:4", "1-2:S8 3:23", "1-2:M8 3:23",
    "1-2:S8 3:19 4:4", "1-2:M8 3:19 4:4", "1-2:S8 3:15 4:8", "1-2:M8 3:15 4:8", "1-2:S8 3:15 4:4 5:4",
    "1-2:M8 3:15 4:4 5:4", "1-2:S8 3:11 4:12", "1-2:M8 3:11 4:12", "1-2:S8 3:11 4:8 5:4",
    "1-2:M8 3:11 4:8 5:4", "1-2:S8 3:11 4:4 5:4 6:4", "1-2:M8 3:11 4:4 5:4 6:4", "1-2:S8 3:7 4:8 5:8",
    "1-2:M8 3:7 4:8 5:8", "1-2:S8 3:7 4:8 5:4 6:4", "1-2:M8 3:7 4:8 5:4 6:4",
};

/* ---- pages ---- */

#define _ SYNTH_NONE
enum { PG_PART, PG_PROGRAM, PG_ENV, PG_LFO, PG_COMMON };
#define NO_SW {_, _, _, _, _, _, _, _}
static const synth_page_t pages[] = {
    [PG_PART] = {0, "PART", {P_MEDIA, P_TONE, P_LEVEL, P_OUT, P_RX_CH, P_KEY_LO, P_KEY_HI, P_BEND}, NO_SW},
    [PG_PROGRAM] = {0, "PROGRAM CHANGE", {P_PGM_MAP, _, _, _, _, _, _, _}, {P_PGM_CHG, _, _, _, _, _, _, _}},
    [PG_ENV] = {1, "ENV / PITCH", {P_ATTACK, P_RELEASE, P_COARSE, P_FINE, P_DETUNE, P_VEL, P_LVL_PRESS,
                                   P_PITCH_PRESS}, NO_SW},
    [PG_LFO] = {2, "LFO", {P_LFO_RATE, P_LFO_DELAY, P_LFO_AUTO_RISE, P_LFO_AUTO_DEPTH, P_LFO_MAN_RISE,
                           P_LFO_MAN_DEPTH, P_LFO_CH_PRESS, P_LFO_POLY}, NO_SW},
    [PG_COMMON] = {3, "COMMON", {P_OUTMODE, P_CHO_RATE, P_CHO_DEPTH, P_TREM_RATE, P_TREM_DEPTH, _, _, _}, NO_SW},
};
#undef _

/* ---- settings ---- */

static const synth_setting_t settings[] = {
    /* The unit's control channel, which is also its device ID */
    [SETTING_CHANNEL] = {"CONTROL CH", SET_CHANNEL, 0, 15, 0},
};

/* ---- state ---- */

static uint8_t patch[PATCH_LEN], init_patch[PATCH_LEN];
static bool dirty[PATCH_LEN];  /* still to send; the name's 10 bytes go as one message */
static bool loaded;
static int part;  /* 0-5: the part edited */

static int byte_of(int p, int pt) { return p < COMMON_N ? COMMON_AT + p : PART_AT + pt * PART_N + (p - COMMON_N); }

/* Until a patch is loaded the values are the init patch's */
static void ensure(void)
{
    if (loaded) {
        return;
    }
    loaded = true;
    FILE *f = fopen(STORAGE_ROOT "/u110/000 Init", "rb");
    if (f) {
        if (fread(init_patch, 1, PATCH_LEN, f) != PATCH_LEN) {
            memset(init_patch, 0, PATCH_LEN);
        }
        fclose(f);
    }
    memcpy(patch, init_patch, PATCH_LEN);
}

static int get(int p) { ensure(); return patch[byte_of(p, part)]; }

/* ---- sending ---- */

static int device_id(void) { return synth_setting(SETTING_CHANNEL) & 0x0F; }

static uint32_t address_of(int at)
{
    if (at < COMMON_AT) {
        return ADDR(0, 1, 0);  /* the name */
    }
    if (at < PART_AT) {
        return ADDR(0, 1, 0x18 + at - COMMON_AT);
    }
    return ADDR(0, 0x10 + (at - PART_AT) / PART_N, (at - PART_AT) % PART_N);
}

static bool send_index(int at)
{
    uint8_t msg[32], data[2 * NAME_LEN];
    int len = 1;
    if (at < COMMON_AT) {
        /* 20 bytes from the first address: each character as its low then high nibble */
        for (int i = 0; i < NAME_LEN; i++) {
            data[2 * i] = patch[i] & 0x0F;
            data[2 * i + 1] = patch[i] >> 4 & 0x0F;
        }
        len = 2 * NAME_LEN;
    } else {
        data[0] = patch[at] & 0x7F;
    }
    uint32_t a = address_of(at);
    int n = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x41;
    msg[n++] = device_id();
    msg[n++] = MODEL_ID;
    msg[n++] = 0x12;
    msg[n++] = a >> 14 & 0x7F;
    msg[n++] = a >> 7 & 0x7F;
    msg[n++] = a & 0x7F;
    for (int i = 0; i < len; i++) {
        msg[n++] = data[i];
    }
    int sum = 0;
    for (int i = 5; i < n; i++) {
        sum += msg[i];
    }
    msg[n++] = (128 - (sum & 0x7F)) & 0x7F;
    msg[n++] = 0xF7;
    if (!midi_send_sysex(msg, n)) {
        return false;
    }
    if (at < COMMON_AT) {
        memset(dirty, 0, COMMON_AT);
    } else {
        dirty[at] = false;
    }
    return true;
}

static int scan;
static int64_t next_send_us;

static void tick(int64_t now)
{
    if (now < next_send_us) {
        return;
    }
    for (int k = 0; k < PATCH_LEN; k++) {
        int at = (scan + k) % PATCH_LEN;
        if (dirty[at]) {
            if (send_index(at)) {
                scan = (at + 1) % PATCH_LEN;
                next_send_us = now + SEND_GAP_US;
                if (at == byte_of(P_OUTMODE, 0)) {
                    /* The unit sets up its voice groups (seen: parts' outputs left OFF): give it
                     * time, then send the parts' outputs again */
                    next_send_us += MODE_PAUSE_US;
                    for (int pt = 0; pt < PARTS; pt++) {
                        dirty[byte_of(P_OUT, pt)] = true;
                    }
                }
            }
            return;
        }
    }
}

/* ---- editing ---- */

static void set_value(int p, int v)
{
    int at = byte_of(p, part);
    patch[at] = v;
    dirty[at] = true;
}

static bool param_step(int p, int steps)
{
    int v = get(p), nv = v + steps;
    nv = nv < params[p].min ? params[p].min : nv > params[p].max ? params[p].max : nv;
    if (nv == v) {
        return false;
    }
    set_value(p, nv);
    return true;
}

static void param_toggle(int p) { set_value(p, !get(p)); }

static bool param_reset(int p)
{
    ensure();
    int v = init_patch[byte_of(p, part)];
    if (get(p) == v) {
        return false;
    }
    set_value(p, v);
    return true;
}

/* ---- formatting ---- */

static const char *param_name(int p) { return params[p].name; }

static void note_name(int v, char *buf, size_t len)
{
    static const char *const notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", notes[v % 12], v / 12 - 1);  /* 60 = C4 */
}

static void media_code(int v, char *buf, size_t len)
{
    if (v) {
        snprintf(buf, len, "C%02d", v);
    } else {
        snprintf(buf, len, "INT");
    }
}

static void param_format(int p, char *buf, size_t len)
{
    const u_param_t *pp = &params[p];
    int v = get(p);
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_PLUS1:
    case K_TONE:
    case K_OUTMODE:
        snprintf(buf, len, "%d", v + 1);
        break;
    case K_SWITCH:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    case K_NOTE:
        note_name(v, buf, len);
        break;
    case K_OUT:
        if (v >= 6) {
            snprintf(buf, len, "OFF");
        } else {
            snprintf(buf, len, "%d", v + 1);
        }
        break;
    case K_POLY:
        snprintf(buf, len, poly_semitones[v & 15] ? "%+d" : "0", poly_semitones[v & 15]);
        break;
    case K_MEDIA:
        media_code(v, buf, len);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    const u_param_t *pp = &params[p];
    if (pp->kind == K_SWITCH || pp->kind == K_MEDIA || pp->kind == K_TONE || pp->kind == K_OUTMODE ||
        pp->kind == K_OUT) {
        return -1;
    }
    return (get(p) - pp->min) * 1000 / (pp->max - pp->min);
}

static bool param_on(int p) { return get(p); }

static int param_units(int p)
{
    int range = params[p].max - params[p].min;
    return range <= 1 ? 80 : range <= 6 ? 70 : range <= 16 ? 40 : range <= 30 ? 25 : 10;
}

/* The tone names of one media (data/u110/tones/NN, one per line), the last
 * one asked for kept */
static char tone_names[TONES][TONE_NAME_LEN];
static int names_media = -1, names_n;

static void load_names(int media)
{
    if (media == names_media) {
        return;
    }
    names_media = media;
    names_n = 0;
    char path[40], line[48];
    snprintf(path, sizeof(path), STORAGE_ROOT "/u110/tones/%02d", media);
    FILE *f = fopen(path, "r");
    while (f && names_n < TONES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(tone_names[names_n++], TONE_NAME_LEN, "%s", line);
    }
    if (f) {
        fclose(f);
    }
}

/* Media, tone and output mode: the code big, what it is below */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    char code[8];
    const char *name = "";
    int v = get(p);
    switch (p) {
    case P_MEDIA:
        media_code(v, code, sizeof(code));
        name = v < 16 ? media_names[v] : "";
        break;
    case P_TONE: {
        int media = patch[byte_of(P_MEDIA, part)];
        load_names(media);
        snprintf(code, sizeof(code), "%d", v + 1);
        name = v < names_n ? tone_names[v] : "";
        break;
    }
    case P_OUTMODE:
        snprintf(code, sizeof(code), "%d", v + 1);
        name = outmode_text[v];
        break;
    default:
        return SYNTH_DRAW_NONE;
    }
    jv880_draw_code_name(disp, x, y, w, code, name);
    return SYNTH_DRAW_FULL;
}

/* ---- parts ---- */

static void select_part(int pt)
{
    part = (pt + PARTS) % PARTS;
    synth_redraw();
}

static void part_step(int d) { select_part(part + (d > 0 ? 1 : -1)); }

/* Shift + Group A-F: part 1-6 */
static bool shift_group(int g)
{
    if (g >= PARTS) {
        return false;
    }
    select_part(g);
    return true;
}

static int shift_group_led(int g) { return g >= PARTS ? -1 : g == part; }

static void header_tag(char *buf, size_t len) { snprintf(buf, len, "PART %d", part + 1); }

/* The pads play the part edited, on its receive channel */
static int play_channel(void) { return get(P_RX_CH) & 0x0F; }

/* ---- patches (library) ---- */

static bool patch_load(const char *path)
{
    ensure();
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[PATCH_LEN];
    bool ok = fread(buf, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memcpy(patch, buf, PATCH_LEN);
    memset(dirty, 1, PATCH_LEN);
    scan = 0;  /* the name and the output mode go first */
    return true;
}

static bool patch_save(const char *path)
{
    ensure();
    /* The patch's own name (10 characters) follows the file's: "NNN Name" */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    for (int i = 0; i < NAME_LEN; i++) {
        char c = *title ? *title++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
    }
    mkdir(STORAGE_ROOT "/u110", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    dirty[0] = true;  /* the unit gets the name too */
    return ok;
}

static int unknown_count(void) { return 0; }

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"U-110", {"Six parts, each playing a PCM tone on", "its own channel.", "A PART: tone, level, key range;",
               "B ENV / PITCH; C LFO; D COMMON.", "SHIFT + A-F: part 1-6 (A = part 1);",
               "STEP arrows: the next part.", "No filter: the tone is the sample."},
     {BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2, BTN_GROUP_A + 3, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT,
      -1}},
    {"TO THE UNIT", {"Every change is sent as it is made;", "a library patch takes a few seconds.",
                     "Set the U-110's SETUP: MIDI EXCLUSIVE", "to ON. Its control channel is the device",
                     "ID: the CONTROL CH setting here.", "(Reading and storing: not yet.)"},
     {BTN_CONTROL, -1, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_u110 = {
    .name = "Roland U-110",
    .key = "u110",
    .patch_dir = STORAGE_ROOT "/u110",
    .default_patch = "000 Init",
    .first_user_patch = 100,
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
    .param_reset = param_reset,
    .param_draw = param_draw,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .unknown_count = unknown_count,
    .play_channel = play_channel,
    .instance_step = part_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .tick = tick,
    .theme_bright = {100, 0, 127},  /* violet */
    .theme_dim = {9, 0, 12},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
