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
 * Reading (Auto: the unit's current patch; needs its MIDI OUT on the Maschine's
 * MIDI IN, never required), IMPORT (its patches 1-64 into the library as
 * 001-064: Program Change, then the temporary patch is read) and writing into
 * its patch memory (REC > TO U-110; the unit's Memory Protect, PATCH > WRT >
 * MEM. P, must be OFF or it ignores it) use bulk dumps: 2 x 128 bytes, every value
 * as two nibbles. The manual doesn't give their layout; Edisyn's
 * RolandU110.java does (it's unverified apart from what is tested here). The
 * SysEx map has no rhythm setup, so "Drums" is just a tone.
 *
 * A patch file is 171 bytes: the name (10), the 5 common parameters and 6 x
 * 26 part parameters, each in the order of its address (00 1n 00 .. 00 1n 19).
 */
#include <dirent.h>
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
static int scan;  /* where the send loop looks for the next value to send */
static int64_t next_send_us;
static bool imp_busy(void);

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

/* One DT1 (data set): up to 128 data bytes at a 3-byte address */
static bool send_dt1(uint32_t a, const uint8_t *data, int len)
{
    uint8_t msg[8 + 128 + 2];
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
    return midi_send_sysex(msg, n);
}

/* A patch value as the two nibbles the unit's name and bulk data use: low, high */
static void put_nibbles(uint8_t *out, int v)
{
    out[0] = v & 0x0F;
    out[1] = v >> 4 & 0x0F;
}

static bool send_index(int at)
{
    uint8_t data[2 * NAME_LEN];
    int len = 1;
    if (at < COMMON_AT) {
        /* 20 bytes from the first address: each character as its low then high nibble */
        for (int i = 0; i < NAME_LEN; i++) {
            put_nibbles(data + 2 * i, patch[i]);
        }
        len = 2 * NAME_LEN;
    } else {
        data[0] = patch[at] & 0x7F;
    }
    if (!send_dt1(address_of(at), data, len)) {
        return false;
    }
    if (at < COMMON_AT) {
        memset(dirty, 0, COMMON_AT);
    } else {
        dirty[at] = false;
    }
    return true;
}

/* ---- bulk dumps ----
 * A patch is 256 bytes in two DT1 packets of 128: 8 unknown bytes, the name
 * (10 characters), the output mode, chorus and tremolo (5 values), 2 unknown,
 * then 6 parts x 16 byte-values, each value as two nibbles (low, high) and
 * several settings sharing a byte (layout: Edisyn's RolandU110.java). The
 * unknown bytes are sent as zeros. */
#define BULK_LEN 256
#define BULK_HALF 128
#define BULK_NAME_AT 8
#define BULK_COMMON_AT (BULK_NAME_AT + 2 * NAME_LEN)
#define BULK_PARTS_AT (BULK_COMMON_AT + 2 * COMMON_N + 2)
#define BULK_VALUES 16  /* byte-values in a part */
#define PATCH_ADDR(slot, half) ADDR(2, (slot) * 2 + (half), 0)  /* patch 0-63 */
#define TEMP_ADDR(half) ADDR(1, 1 + (half), 0)

typedef struct {
    uint8_t value, off, shift, mask;  /* a part's parameter (address offset) in byte-value `value` */
} bulk_field_t;

static const bulk_field_t bulk_fields[] = {
    {0, 0x02, 0, 0x1F}, {0, 0x19, 5, 7},   /* media, LFO poly press */
    {1, 0x03, 0, 0x7F},                    /* tone */
    {2, 0x01, 0, 15},   {2, 0x04, 4, 15},  /* rx channel, bend range */
    {3, 0x05, 0, 0x7F}, {4, 0x06, 0, 0x7F}, {5, 0x07, 0, 0x7F},  /* key lo, hi, level */
    {6, 0x08, 0, 15},   {6, 0x09, 4, 15},  /* velocity, level press sens */
    {7, 0x0A, 0, 15},   {7, 0x0B, 4, 15},  /* attack, release */
    {8, 0x0C, 0, 0x7F}, {9, 0x0D, 0, 0x7F},  /* coarse, fine */
    {10, 0x17, 0, 15},  {10, 0x18, 4, 15}, /* detune, pitch poly press */
    {11, 0x16, 0, 7},   {11, 0x15, 3, 1}, {11, 0x00, 5, 7},  /* program map, program change, output */
    {12, 0x0E, 0, 15},                     /* LFO rate */
    {13, 0x11, 0, 15},  {13, 0x13, 4, 15}, /* LFO auto depth, manual depth */
    {14, 0x10, 0, 15},  {14, 0x12, 4, 15}, /* LFO auto rise, manual rise */
    {15, 0x14, 0, 15},  {15, 0x0F, 4, 15}, /* LFO ch press sens, auto delay */
};

static void pack_bulk(const uint8_t *pt, uint8_t *out)
{
    memset(out, 0, BULK_LEN);
    for (int i = 0; i < NAME_LEN; i++) {
        put_nibbles(out + BULK_NAME_AT + 2 * i, pt[i]);
    }
    for (int k = 0; k < COMMON_N; k++) {
        put_nibbles(out + BULK_COMMON_AT + 2 * k, pt[COMMON_AT + k]);
    }
    for (int n = 0; n < PARTS; n++) {
        const uint8_t *pp = pt + PART_AT + n * PART_N;
        uint8_t raw[BULK_VALUES] = {0};
        for (size_t f = 0; f < sizeof(bulk_fields) / sizeof(bulk_fields[0]); f++) {
            const bulk_field_t *bf = &bulk_fields[f];
            raw[bf->value] |= (pp[bf->off] & bf->mask) << bf->shift;
        }
        for (int v = 0; v < BULK_VALUES; v++) {
            put_nibbles(out + BULK_PARTS_AT + n * 2 * BULK_VALUES + 2 * v, raw[v]);
        }
    }
}

static int nibbles(const uint8_t *in) { return (in[0] & 0x0F) | (in[1] & 0x0F) << 4; }

static void unpack_bulk(const uint8_t *in, uint8_t *pt)
{
    for (int i = 0; i < NAME_LEN; i++) {
        int c = nibbles(in + BULK_NAME_AT + 2 * i);
        pt[i] = c >= 32 && c < 127 ? c : ' ';
    }
    for (int k = 0; k < COMMON_N; k++) {
        pt[COMMON_AT + k] = nibbles(in + BULK_COMMON_AT + 2 * k) & (k ? 15 : 63);
    }
    for (int n = 0; n < PARTS; n++) {
        uint8_t *pp = pt + PART_AT + n * PART_N;
        uint8_t raw[BULK_VALUES];
        for (int v = 0; v < BULK_VALUES; v++) {
            raw[v] = nibbles(in + BULK_PARTS_AT + n * 2 * BULK_VALUES + 2 * v);
        }
        memset(pp, 0, PART_N);
        for (size_t f = 0; f < sizeof(bulk_fields) / sizeof(bulk_fields[0]); f++) {
            const bulk_field_t *bf = &bulk_fields[f];
            pp[bf->off] = raw[bf->value] >> bf->shift & bf->mask;
        }
    }
}

/* ---- reading back (optional: needs the U-110's MIDI OUT on the Maschine's
 * MIDI IN) ---- */

static uint8_t bulk_in[BULK_LEN];
static uint8_t bulk_have;  /* bit n = half n is in */
static bool read_asked;    /* Auto pressed: the reply replaces the patch */
static bool from_unit;     /* the sound is the unit's own (read), not a library patch */
static uint8_t as_read[PATCH_LEN];  /* as the unit last gave it, for Rec + Erase */
static bool as_read_ok;
#define READ_CHECK_US 500000
#define READ_TRIES 3
static int read_tries;
static int64_t read_check_at;  /* when to ask again if nothing came, 0 = not waiting */

/* RQ1 for the temporary patch: 256 bytes, answered by two DT1s */
static void request_current(void)
{
    uint8_t msg[13] = {0xF0, 0x41, device_id(), MODEL_ID, 0x11, 1, 1, 0, 0, 2, 0};
    int sum = 0;
    for (int i = 5; i < 11; i++) {
        sum += msg[i];
    }
    msg[11] = (128 - (sum & 0x7F)) & 0x7F;
    msg[12] = 0xF7;
    midi_send_sysex(msg, sizeof(msg));
    bulk_have = 0;
}

static void read_patch(void)
{
    if (imp_busy()) {
        return;
    }
    read_asked = true;
    as_read_ok = false;
    read_tries = 1;
    read_check_at = esp_timer_get_time() + READ_CHECK_US;
    request_current();
}

/* ---- import: the unit's patches 1-64 into the library ----
 * For each: a Program Change on the control channel (needs SETUP: PGM CHANGE
 * ON), then the temporary patch is read - the stored ones can't be asked for
 * directly (Edisyn). Saved as 001-064, replacing the earlier import; they're
 * protected (own patches start at FIRST_USER_PATCH). */
#define FIRST_USER_PATCH 100
#define UNIT_PATCHES 64
#define IMPORT_PC_WAIT_US 300000
#define IMPORT_TIMEOUT_US 1000000
#define IMPORT_TRIES 3
typedef enum { IMP_IDLE, IMP_PC, IMP_ASK, IMP_REPLY } imp_stage_t;
static imp_stage_t imp_stage;
static int imp_slot, imp_saved, imp_tries;
static int64_t imp_at;

/* The names of the library's 001-064, shown when picking where to store */
static char slot_names[UNIT_PATCHES][NAME_LEN + 1];
static bool slots_scanned;

static void scan_slots(void)
{
    if (slots_scanned) {
        return;
    }
    slots_scanned = true;
    memset(slot_names, 0, sizeof(slot_names));
    DIR *dir = opendir(STORAGE_ROOT "/u110");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        const char *sp = strchr(e->d_name, ' ');
        if (n >= 1 && n <= UNIT_PATCHES && sp) {
            snprintf(slot_names[n - 1], sizeof(slot_names[0]), "%s", sp + 1);
        }
    }
    if (dir) {
        closedir(dir);
    }
}

static bool imp_busy(void) { return imp_stage != IMP_IDLE; }

static void import_finish(const char *why)
{
    imp_stage = IMP_IDLE;
    slots_scanned = false;
    /* The unit's temporary patch is the last one read: send ours again */
    memset(dirty, 1, PATCH_LEN);
    scan = 0;
    synth_patches_changed();
    char buf[48];
    if (why) {
        snprintf(buf, sizeof(buf), "%s", why);
    } else {
        snprintf(buf, sizeof(buf), "IMPORTED %d PATCHES", imp_saved);
    }
    synth_notice(buf);
    ESP_LOGI(TAG, "import: %s (%d saved)", why ? why : "done", imp_saved);
}

static void import_save(const uint8_t *pt)
{
    char name[NAME_LEN + 1];
    memcpy(name, pt, NAME_LEN);
    name[NAME_LEN] = 0;
    for (int i = NAME_LEN - 1; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    for (char *c = name; *c; c++) {
        if (*c == '/') {
            *c = '_';
        }
    }
    char path[96];
    snprintf(path, sizeof(path), STORAGE_ROOT "/u110/%03d %s", imp_slot + 1, name[0] ? name : "Patch");
    FILE *f = fopen(path, "wb");
    if (f) {
        imp_saved += fwrite(pt, 1, PATCH_LEN, f) == PATCH_LEN;
        fclose(f);
    }
}

static void import_start(void)
{
    if (imp_busy()) {
        return;
    }
    mkdir(STORAGE_ROOT "/u110", 0777);
    DIR *dir = opendir(STORAGE_ROOT "/u110");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        if (n >= 1 && n <= UNIT_PATCHES) {
            char path[300];
            snprintf(path, sizeof(path), STORAGE_ROOT "/u110/%s", e->d_name);
            remove(path);  /* the earlier import */
        }
    }
    if (dir) {
        closedir(dir);
    }
    memset(dirty, 0, PATCH_LEN);
    read_check_at = 0;
    imp_slot = imp_saved = 0;
    imp_at = 0;
    imp_stage = IMP_PC;
    synth_notice("IMPORT: asking the U-110...");
}

/* ---- writing into the unit's own memory (REC > TO U-110) ----
 * The patch goes to patch memory 1-64 as two DT1s, a pause between */
#define STORE_GAP_US 150000
static uint8_t store_buf[BULK_LEN];
static int store_slot;
static uint8_t store_left;  /* bit n = half n still to send */

static void unit_slot_name(int slot, char *buf, size_t len) { snprintf(buf, len, "P%02d", slot + 1); }

static void unit_slot_info(int slot, char *buf, size_t len)
{
    scan_slots();
    snprintf(buf, len, "%s", slot_names[slot]);
}

static void unit_store(int slot, const char *name)
{
    ensure();
    for (int i = 0; i < NAME_LEN; i++) {
        char c = *name ? *name++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
    }
    dirty[0] = true;  /* the temporary patch gets the name too */
    pack_bulk(patch, store_buf);
    store_slot = slot;
    store_left = 3;
    ESP_LOGI(TAG, "store P%02d \"%.*s\"", slot + 1, NAME_LEN, (const char *)patch);
    scan_slots();
    snprintf(slot_names[slot], sizeof(slot_names[0]), "%.*s", NAME_LEN, (const char *)patch);
    for (int i = (int)strlen(slot_names[slot]) - 1; i >= 0 && slot_names[slot][i] == ' '; i--) {
        slot_names[slot][i] = 0;
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "WRITING TO P%02d", slot + 1);
    synth_notice(buf);
}

/* ---- the send loop ---- */

static void tick(int64_t now)
{
    if (imp_stage != IMP_IDLE) {
        if (imp_stage == IMP_PC && now >= imp_at) {
            midi_send(0xC0 | device_id(), imp_slot, 0);
            imp_stage = IMP_ASK;
            imp_tries = 0;
            imp_at = now + IMPORT_PC_WAIT_US;
        } else if (imp_stage == IMP_ASK && now >= imp_at) {
            request_current();
            imp_stage = IMP_REPLY;
            imp_at = now + IMPORT_TIMEOUT_US;
        } else if (imp_stage == IMP_REPLY && now > imp_at) {
            if (++imp_tries < IMPORT_TRIES) {
                imp_stage = IMP_ASK;  /* the first request after boot often gets lost */
                imp_at = now;
            } else {
                import_finish("IMPORT stopped: no reply - U-110 MIDI OUT connected?");
            }
        }
        return;
    }
    if (read_check_at && now > read_check_at) {
        if (bulk_have != 3 && read_tries < READ_TRIES) {
            read_tries++;
            request_current();
            read_check_at = now + READ_CHECK_US;
        } else {
            read_check_at = 0;
        }
    }
    if (now < next_send_us) {
        return;
    }
    if (store_left) {
        int half = __builtin_ctz(store_left);
        bool sent = send_dt1(PATCH_ADDR(store_slot, half), store_buf + half * BULK_HALF, BULK_HALF);
        ESP_LOGI(TAG, "store P%02d half %d: %s (dev %d, addr 02 %02X 00)", store_slot + 1, half,
                 sent ? "sent" : "queue full", device_id(), store_slot * 2 + half);
        if (sent) {
            store_left &= ~(1 << half);
            next_send_us = now + STORE_GAP_US;
        }
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

/* The whole patch is in (both halves) */
static void bulk_complete(void)
{
    uint8_t pt[PATCH_LEN];
    unpack_bulk(bulk_in, pt);
    bulk_have = 0;
    read_check_at = 0;
    if (imp_stage == IMP_REPLY) {
        import_save(pt);
        char buf[32];
        snprintf(buf, sizeof(buf), "IMPORT %d / %d", imp_slot + 1, UNIT_PATCHES);
        synth_notice(buf);
        if (++imp_slot >= UNIT_PATCHES) {
            import_finish(NULL);
        } else {
            imp_stage = IMP_PC;
            imp_at = esp_timer_get_time() + 50000;
        }
        return;
    }
    ensure();
    memcpy(patch, pt, PATCH_LEN);
    memset(dirty, 0, PATCH_LEN);  /* what was still to send is the unit's now */
    from_unit = true;
    synth_state_received();
    if (read_asked) {
        /* Auto (read) on purpose: the unit's patch replaces whatever library patch was loaded */
        read_asked = false;
        memcpy(as_read, patch, PATCH_LEN);
        as_read_ok = true;
        synth_sound_replaced();
    }
    synth_redraw();
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (len < 11 || d[0] != 0xF0 || d[1] != 0x41 || d[2] != device_id() || d[3] != MODEL_ID || d[4] != 0x12 ||
        d[len - 1] != 0xF7) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    int n = (int)len - 10, sum = 0;
    for (size_t i = 5; i < len - 2; i++) {
        sum += d[i];
    }
    if (((128 - (sum & 0x7F)) & 0x7F) != d[len - 2]) {
        ESP_LOGW(TAG, "SysEx ignored: bad checksum");
        return;
    }
    uint32_t addr = ADDR(d[5], d[6], d[7]);
    const uint8_t *data = d + 8;
    for (int half = 0; half < 2; half++) {
        if (n == BULK_HALF && addr == TEMP_ADDR(half)) {
            memcpy(bulk_in + half * BULK_HALF, data, BULK_HALF);
            bulk_have |= 1 << half;
            if (bulk_have == 3) {
                bulk_complete();
            }
            return;
        }
    }
    /* A value edited on the unit's panel (it sends these when SETUP: MIDI EXCLUSIVE is ON) */
    if (n == 1 && imp_stage == IMP_IDLE) {
        int at = -1;
        if (addr >= ADDR(0, 1, 0x18) && addr < ADDR(0, 1, 0x18 + COMMON_N)) {
            at = COMMON_AT + (int)(addr - ADDR(0, 1, 0x18));
        } else if (addr >= ADDR(0, 0x10, 0) && addr < ADDR(0, 0x10 + PARTS, 0) && (addr & 0x7F) < PART_N) {
            at = PART_AT + (int)((addr >> 7) - 0x10) * PART_N + (int)(addr & 0x7F);
        }
        if (at >= 0) {
            ensure();
            patch[at] = data[0];
            synth_redraw();
        }
    }
}

/* Rec + Erase on the unit's own sound: back to what it was when read */
static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(patch, as_read, PATCH_LEN);
    memset(dirty, 1, PATCH_LEN);
    scan = 0;
    return true;
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

/* ---- patch overview (Grid): the six parts as a table, like the manual's patch setting chart ---- */

static void overview_draw(int page, int disp)
{
    (void)page;
    ensure();
    char buf[64], keys[2][8];
    int mode = patch[byte_of(P_OUTMODE, 0)];
    if (disp == 0) {
        snprintf(buf, sizeof(buf), "PATCH %.10s  out mode %d: %s", (const char *)patch, mode + 1,
                 outmode_text[mode < 50 ? mode : 49]);
        display_text(0, 1, 0, &font_small, buf, true);
    } else {
        snprintf(buf, sizeof(buf), "CHO %d/%d  TREM %d/%d", patch[byte_of(P_CHO_RATE, 0)], patch[byte_of(P_CHO_DEPTH, 0)],
                 patch[byte_of(P_TREM_RATE, 0)], patch[byte_of(P_TREM_DEPTH, 0)]);
        display_text(1, 1, 0, &font_small, buf, true);
        display_text(1, 200, 0, &font_small, "Grid: close", true);
    }
    display_fill_rect(disp, 0, 11, 255, 1, true);
    for (int pt = 0; pt < PARTS; pt++) {
        int y = 13 + pt * 8, out = patch[byte_of(P_OUT, pt)];
        char mark = pt == part ? '>' : ' ';
        if (out >= 6) {  /* output off: the part isn't used */
            if (disp == 0) {
                snprintf(buf, sizeof(buf), "%c%d  --  (off)", mark, pt + 1);
                display_text(0, 1, y, &font_tiny, buf, true);
            }
            continue;
        }
        int media = patch[byte_of(P_MEDIA, pt)], tone = patch[byte_of(P_TONE, pt)];
        if (disp == 0) {
            char m[8];
            media_code(media, m, sizeof(m));
            load_names(media);
            snprintf(buf, sizeof(buf), "%c%d %s %02d %-15.15s LV%3d CH%2d", mark, pt + 1, m, tone + 1,
                     tone < names_n ? tone_names[tone] : "", patch[byte_of(P_LEVEL, pt)],
                     patch[byte_of(P_RX_CH, pt)] + 1);
        } else {
            note_name(patch[byte_of(P_KEY_LO, pt)], keys[0], sizeof(keys[0]));
            note_name(patch[byte_of(P_KEY_HI, pt)], keys[1], sizeof(keys[1]));
            snprintf(buf, sizeof(buf), "%s-%s OUT%d BND%d ATK%+d REL%+d TUN%+d/%+d", keys[0], keys[1], out + 1,
                     patch[byte_of(P_BEND, pt)], patch[byte_of(P_ATTACK, pt)] - 8, patch[byte_of(P_RELEASE, pt)] - 8,
                     patch[byte_of(P_COARSE, pt)] - 64, patch[byte_of(P_FINE, pt)] - 64);
        }
        display_text(disp, 1, y, &font_tiny, buf, true);
    }
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
    from_unit = as_read_ok = false;
    read_check_at = 0;  /* a read still going would bring the unit's old patch back */
    bulk_have = 0;
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

/* The sound's own name when it is the unit's (read back) */
static bool sound_name(char *buf, size_t len)
{
    if (!from_unit) {
        return false;
    }
    snprintf(buf, len, "%.*s", NAME_LEN, (const char *)patch);
    for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) {
        buf[i] = 0;
    }
    return true;
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"U-110", {"Six parts, each playing a PCM tone on", "its own channel.", "A PART: tone, level, key range;",
               "B ENV / PITCH; C LFO; D COMMON.", "SHIFT + A-F: part 1-6 (A = part 1);",
               "STEP arrows: the next part.", "No filter: the tone is the sample."},
     {BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2, BTN_GROUP_A + 3, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT,
      -1}},
    {"TO THE UNIT", {"Every change is sent as it is made;", "a library patch takes a few seconds.",
                     "Set the U-110's SETUP: MIDI EXCLUSIVE", "to ON. Its control channel is the device",
                     "ID: the CONTROL CH setting here.", "Panel edits show up here too."},
     {BTN_CONTROL, -1, -1, -1, -1, -1, -1, -1}},
    {"READ, WRITE", {"AUTO: read the unit's patch (its MIDI", "OUT to the Maschine's MIDI IN).",
                     "REC > TO U-110 (twice): write it into", "the unit's patch 1-64 (its PATCH > WRT >",
                     "MEM. P must be OFF). CONTROL > IMPORT:", "its patches into the library as 001-064",
                     "(needs SETUP: PGM CHANGE ON)."},
     {BTN_AUTO, BTN_REC, BTN_CONTROL, -1, -1, -1, -1, -1}},
};

const synth_t synth_u110 = {
    .name = "Roland U-110",
    .key = "u110",
    .patch_dir = STORAGE_ROOT "/u110",
    .default_patch = "000 Init",
    .first_user_patch = FIRST_USER_PATCH,
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
    .revert = revert,
    .keep_state = true,  /* selecting it reads the unit's patch, sends nothing */
    .play_channel = play_channel,
    .instance_step = part_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_patch,
    .sysex_in = sysex_in,
    .tick = tick,
    .overview_pages = 1,
    .overview_draw = overview_draw,
    .action_name = "IMPORT",
    .action = import_start,
    .unit_store_label = "TO U-110",
    .unit_store_slots = UNIT_PATCHES,
    .unit_slot_name = unit_slot_name,
    .unit_slot_info = unit_slot_info,
    .unit_store = unit_store,
    .theme_bright = {100, 0, 127},  /* violet */
    .theme_dim = {9, 0, 12},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
