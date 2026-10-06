/*
 * Roland JV-880, rhythm level: the Rhythm Setup - a drum kit of 61 keys
 * (C2-C7, 36-96), each key its own sound with a complete tone of its own:
 * wave, pitch envelope, TVF, TVA and output (manual p. 229-230). The unit
 * plays it as the performance's part 8, so this entry puts the unit into
 * Performance mode.
 *
 * Group A-F: A WAVE (wave, tuning, mute group), B PITCH ENV, C TVF, D TVA,
 * E OUTPUT, F KIT (the waves of 8 keys at once, one per knob). The key
 * edited is the one last hit on the pads, or stepped to with the transport
 * arrows; each block's seldom used parameters are on its second page.
 *
 * Every change goes out at once as a DT1 to the Temporary Rhythm Setup.
 * Reading it back (Auto) is optional and needs the JV-880's MIDI OUT on the
 * Maschine's MIDI IN: values are "--" until a kit is loaded, read or the
 * parameter is changed. A kit file is the setup: 61 x 52 bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "jv880r";

#define KEYS 61
#define FIRST_KEY 36  /* C2 */
#define NOTE_LEN 0x34
#define SETUP_LEN (KEYS * NOTE_LEN)
#define TEMP_RHYTHM JV880_ADDR(0x00, 0x07, 0x40, 0x00)
#define INTERNAL_RHYTHM JV880_ADDR(0x01, 0x7F, 0x40, 0x00)
#define PART8_RX JV880_ADDR(0x00, 0x00, 0x1F, 0x15)  /* performance part 8: receive switch, channel */
#define WAVE_UNITS 13  /* encoder counter units per wave number step */

/* ---- parameters ---- */

typedef enum {
    K_NUM,      /* as is */
    K_SIGNED,   /* value - center */
    K_PLUS1,    /* stored 0.., shown 1.. */
    K_LIST,     /* labels[value - min] */
    K_SWITCH,
    K_PAN,      /* 0-127 = L64..63R, 128 = RND */
    K_NOTE,     /* MIDI note, shown as a note name */
    K_MUTE,     /* 0 = OFF, 1-31 */
    K_EG,       /* envelope time/level: value only, the envelope is drawn */
} kind_t;

typedef struct {
    const char *name;  /* fits a 64 px display column */
    uint8_t off;       /* in the key's 52 bytes */
    kind_t kind;
    int16_t min, max, center;
    const char *const *labels;
    bool wide;         /* two nibble bytes (off = high, off + 1 = low) */
} jv_param_t;

static const char *const group_labels[] = {"INT", "EXP", "PCM"};
static const char *const random_labels[] = {"0", "5", "10", "20", "30", "40", "50", "70",
                                            "100", "200", "300", "400", "500", "600", "800", "1200"};
static const char *const sense_labels[] = {"-100", "-70", "-50", "-40", "-30", "-20", "-10", "0",
                                           "+10", "+20", "+30", "+40", "+50", "+70", "+100"};
static const char *const filter_labels[] = {"OFF", "LPF", "HPF"};
static const char *const reso_labels[] = {"SOFT", "HARD"};
static const char *const output_labels[] = {"MAIN", "SUB"};

enum {
    P_GROUP, P_WAVE, P_TONE_SW, P_COARSE, P_MUTE, P_ENV_MODE, P_FINE, P_RANDOM, P_BEND,
    P_PE_VELO, P_PE_TIME, P_PE_DEPTH,
    P_PT1, P_PL1, P_PT2, P_PL2, P_PT3, P_PL3, P_PT4, P_PL4,
    P_F_MODE, P_CUTOFF, P_RESO, P_RESO_MODE, P_F_VELO, P_F_TIME, P_F_DEPTH,
    P_FT1, P_FL1, P_FT2, P_FL2, P_FT3, P_FL3, P_FT4, P_FL4,
    P_LEVEL, P_PAN, P_A_VELO, P_A_TIME,
    P_AT1, P_AL1, P_AT2, P_AL2, P_AT3, P_AL3, P_AT4,
    P_DRY, P_REV_SEND, P_CHO_SEND, P_OUT_SEL,
    PARAM_COUNT,
    P_KIT0 = PARAM_COUNT,  /* 8 keys' waves from the selected one */
    P_KIT7 = P_KIT0 + 7,
    TOTAL_PARAMS
};

#define RP(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, false}
#define RW(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, true}

static const jv_param_t params[PARAM_COUNT] = {
    [P_GROUP] = RP("WAVE GROUP", 0x00, K_LIST, 0, 1, 0, group_labels),  /* no PCM (data card) waves: not tested */
    [P_WAVE] = RW("WAVE NUMBER", 0x01, K_PLUS1, 0, 254, 0, 0),
    [P_TONE_SW] = RP("TONE", 0x03, K_SWITCH, 0, 1, 0, 0),
    [P_COARSE] = RP("COARSE", 0x04, K_NOTE, 0, 127, 0, 0),
    [P_MUTE] = RP("MUTE GROUP", 0x05, K_MUTE, 0, 31, 0, 0),
    [P_ENV_MODE] = RP("SUSTAIN", 0x06, K_SWITCH, 0, 1, 0, 0),
    [P_FINE] = RP("FINE", 0x07, K_SIGNED, 14, 114, 64, 0),
    [P_RANDOM] = RP("RANDOM", 0x08, K_LIST, 0, 15, 0, random_labels),
    [P_BEND] = RP("BEND RANGE", 0x09, K_NUM, 0, 12, 0, 0),
    [P_PE_VELO] = RP("VELO LEVEL", 0x0A, K_SIGNED, 1, 127, 64, 0),
    [P_PE_TIME] = RP("VELO TIME", 0x0B, K_LIST, 0, 14, 0, sense_labels),
    [P_PE_DEPTH] = RP("PENV DEPTH", 0x0C, K_SIGNED, 52, 76, 64, 0),
    [P_PT1] = RP("T1", 0x0D, K_EG, 0, 127, 0, 0),
    [P_PL1] = RP("L1", 0x0E, K_EG, 1, 127, 64, 0),
    [P_PT2] = RP("T2", 0x0F, K_EG, 0, 127, 0, 0),
    [P_PL2] = RP("L2", 0x10, K_EG, 1, 127, 64, 0),
    [P_PT3] = RP("T3", 0x11, K_EG, 0, 127, 0, 0),
    [P_PL3] = RP("L3", 0x12, K_EG, 1, 127, 64, 0),
    [P_PT4] = RP("T4", 0x13, K_EG, 0, 127, 0, 0),
    [P_PL4] = RP("L4", 0x14, K_EG, 1, 127, 64, 0),

    [P_F_MODE] = RP("FILTER", 0x15, K_LIST, 0, 2, 0, filter_labels),
    [P_CUTOFF] = RP("CUTOFF", 0x16, K_NUM, 0, 127, 0, 0),
    [P_RESO] = RP("RESONANCE", 0x17, K_NUM, 0, 127, 0, 0),
    [P_RESO_MODE] = RP("RESO MODE", 0x18, K_LIST, 0, 1, 0, reso_labels),
    [P_F_VELO] = RP("VELO LEVEL", 0x19, K_SIGNED, 1, 127, 64, 0),
    [P_F_TIME] = RP("VELO TIME", 0x1A, K_LIST, 0, 14, 0, sense_labels),
    [P_F_DEPTH] = RP("ENV DEPTH", 0x1B, K_SIGNED, 1, 127, 64, 0),
    [P_FT1] = RP("T1", 0x1C, K_EG, 0, 127, 0, 0),
    [P_FL1] = RP("L1", 0x1D, K_EG, 0, 127, 0, 0),
    [P_FT2] = RP("T2", 0x1E, K_EG, 0, 127, 0, 0),
    [P_FL2] = RP("L2", 0x1F, K_EG, 0, 127, 0, 0),
    [P_FT3] = RP("T3", 0x20, K_EG, 0, 127, 0, 0),
    [P_FL3] = RP("L3", 0x21, K_EG, 0, 127, 0, 0),
    [P_FT4] = RP("T4", 0x22, K_EG, 0, 127, 0, 0),
    [P_FL4] = RP("L4", 0x23, K_EG, 0, 127, 0, 0),

    [P_LEVEL] = RP("LEVEL", 0x24, K_NUM, 0, 127, 0, 0),
    [P_PAN] = RW("PAN", 0x25, K_PAN, 0, 128, 0, 0),
    [P_A_VELO] = RP("VELO LEVEL", 0x27, K_SIGNED, 1, 127, 64, 0),
    [P_A_TIME] = RP("VELO TIME", 0x28, K_LIST, 0, 14, 0, sense_labels),
    [P_AT1] = RP("T1", 0x29, K_EG, 0, 127, 0, 0),
    [P_AL1] = RP("L1", 0x2A, K_EG, 0, 127, 0, 0),
    [P_AT2] = RP("T2", 0x2B, K_EG, 0, 127, 0, 0),
    [P_AL2] = RP("L2", 0x2C, K_EG, 0, 127, 0, 0),
    [P_AT3] = RP("T3", 0x2D, K_EG, 0, 127, 0, 0),
    [P_AL3] = RP("L3", 0x2E, K_EG, 0, 127, 0, 0),
    [P_AT4] = RP("T4", 0x2F, K_EG, 0, 127, 0, 0),

    [P_DRY] = RP("DRY LEVEL", 0x30, K_NUM, 0, 127, 0, 0),
    [P_REV_SEND] = RP("REVERB SEND", 0x31, K_NUM, 0, 127, 0, 0),
    [P_CHO_SEND] = RP("CHORUS SEND", 0x32, K_NUM, 0, 127, 0, 0),
    [P_OUT_SEL] = RP("OUTPUT", 0x33, K_LIST, 0, 1, 0, output_labels),
};

/* ---- pages ---- */

#define _ SYNTH_NONE
enum { PG_WAVE, PG_PITCH, PG_PENV, PG_TVF, PG_TVF_ENV, PG_TVA, PG_TVA_ENV, PG_OUTPUT, PG_KIT };
#define NO_SW {_, _, _, _, _, _, _, _}
static const synth_page_t pages[] = {
    /* A WAVE: the sound and its tuning; the switches over the knobs they belong to */
    [PG_WAVE] = {0, "WAVE", {P_GROUP, P_WAVE, P_COARSE, P_FINE, P_RANDOM, P_BEND, P_MUTE, _},
                 {P_TONE_SW, _, _, _, _, _, P_ENV_MODE, _}},
    /* B PITCH ENV, C TVF, D TVA: the same two-page shape (settings, then the envelope) */
    [PG_PITCH] = {1, "PITCH ENV", {P_PE_DEPTH, P_PE_VELO, P_PE_TIME, _, _, _, _, _}, NO_SW, 1},
    [PG_PENV] = {1, "PITCH ENV", {P_PT1, P_PL1, P_PT2, P_PL2, P_PT3, P_PL3, P_PT4, P_PL4}, NO_SW, 1},
    [PG_TVF] = {2, "TVF", {P_F_MODE, P_CUTOFF, P_RESO, P_RESO_MODE, P_F_DEPTH, P_F_VELO, P_F_TIME, _}, NO_SW, 1},
    [PG_TVF_ENV] = {2, "TVF ENV", {P_FT1, P_FL1, P_FT2, P_FL2, P_FT3, P_FL3, P_FT4, P_FL4}, NO_SW, 1},
    [PG_TVA] = {3, "TVA", {P_LEVEL, P_PAN, P_A_VELO, P_A_TIME, _, _, _, _}, NO_SW, 1},
    [PG_TVA_ENV] = {3, "TVA ENV", {P_AT1, P_AL1, P_AT2, P_AL2, P_AT3, P_AL3, P_AT4, _}, NO_SW, 1},
    /* E OUTPUT */
    [PG_OUTPUT] = {4, "OUTPUT", {P_DRY, P_REV_SEND, P_CHO_SEND, P_OUT_SEL, _, _, _, _}, NO_SW},
    /* F KIT: 8 keys from the selected one, the way the pads go (Pad Mode scale) */
    [PG_KIT] = {5, "KIT", {P_KIT0, P_KIT0 + 1, P_KIT0 + 2, P_KIT0 + 3, P_KIT0 + 4, P_KIT0 + 5, P_KIT0 + 6, P_KIT7},
                NO_SW},
};
#undef _

/* ---- settings ---- */

#define SETTING_UNIT 1
#define SETTING_CARD 2
#define SETTING_VOLUME_JV 3
static const synth_setting_t settings[] = {
    /* The pads play the rhythm part on this until part 8's channel is read */
    [SETTING_CHANNEL] = {"RHYTHM CH", SET_CHANNEL, 0, 15, 9},
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
    /* The expansion board in the unit, for the names of its waves (as JV-880 PATCH) */
    [SETTING_CARD] = {"CARD", SET_NUMBER, 0, 22, 0},
    [SETTING_VOLUME_JV] = {"VOLUME", SET_NUMBER, 0, 127, 100},
};

static void setting_note(int setting, char *buf, size_t len)
{
    if (setting == SETTING_CARD) {
        snprintf(buf, len, "%s", jv880_card_names[synth_setting(SETTING_CARD) % 23]);
    }
}

static int card_number(void) { return jv880_card_number(synth_setting(SETTING_CARD)); }

/* ---- state ---- */

static uint8_t setup[SETUP_LEN], init_setup[SETUP_LEN];
static uint8_t as_read[SETUP_LEN];  /* the kit as the unit last gave it, for Rec + Erase */
static bool as_read_ok;
static bool known[SETUP_LEN];
static bool init_read;
static int key;  /* the selected key's index, 0 = C2 */
static bool part8_known;
static uint8_t part8_switch, part8_channel;

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }

static void read_init(void)
{
    if (init_read) {
        return;
    }
    FILE *f = fopen(STORAGE_ROOT "/jv880r/000 Init", "rb");
    if (f) {
        init_read = fread(init_setup, 1, SETUP_LEN, f) == SETUP_LEN;
        fclose(f);
    }
}

/* The key and parameter a parameter index means: the KIT knobs are the wave
 * of the keys the pads play from the selected one */
static bool resolve(int p, int *k, int *real)
{
    if (p >= P_KIT0) {
        int note = synth_scale_note(FIRST_KEY + key, p - P_KIT0);
        if (note < FIRST_KEY || note >= FIRST_KEY + KEYS) {
            return false;
        }
        *k = note - FIRST_KEY;
        *real = P_WAVE;
        return true;
    }
    *k = key;
    *real = p;
    return true;
}

static int index_of(int real, int k) { return k * NOTE_LEN + params[real].off; }

static int get(const uint8_t *buf, int real, int k)
{
    int at = index_of(real, k);
    return params[real].wide ? buf[at] << 4 | buf[at + 1] : buf[at];
}

static bool value_known(int p)
{
    int k, real;
    return resolve(p, &k, &real) && known[index_of(real, k)];
}

static int value_or_init(int real, int k)
{
    if (known[index_of(real, k)]) {
        return get(setup, real, k);
    }
    read_init();
    return get(init_setup, real, k);
}

/* ---- sending ---- */

static uint32_t address_of(uint32_t base, int at) { return base + JV880_ADDR(0, 0, at / NOTE_LEN, at % NOTE_LEN); }

static void send_bytes_to(uint32_t base, int from, int len)
{
    uint8_t msg[13 + NOTE_LEN];
    int n = jv880_dt1(device_id(), address_of(base, from), setup + from, len, msg);
    midi_send_sysex(msg, n);
}

/* Whole kits go out a key at a time from tick(), so the MIDI queue never
 * overflows and the unit gets time for each */
static int send_next = KEYS;  /* next key to send, KEYS = idle */
static uint32_t send_base = TEMP_RHYTHM;
static int64_t next_send_us;
#define SEND_GAP_US 12000
#define BYTE_US 330  /* 31250 baud DIN */

static void send_kit(uint32_t base)
{
    send_next = 0;
    send_base = base;
    next_send_us = 0;
}

/* ---- reading back (optional: needs the JV-880's MIDI OUT on the
 * Maschine's MIDI IN) ---- */

/* A read asks for the keys one by one, then part 8's receive channel; ones
 * that didn't arrive (the first request after boot often gets lost) are
 * asked for again, twice at most */
#define READ_ITEMS (KEYS + 1)
#define ALL_ITEMS ((1ULL << READ_ITEMS) - 1)
#define READ_GAP_US 60000
#define READ_CHECK_US 400000
#define READ_TRIES 3
static bool read_asked;  /* Auto pressed: the reply replaces the kit */
static int read_item = READ_ITEMS;  /* next to ask for, READ_ITEMS = idle */
static uint64_t read_got;
static int read_tries;
static int64_t read_check_at;  /* when to see what's missing, 0 = not waiting */

static void request_item(int item)
{
    uint8_t msg[17];
    int n = item < KEYS ? jv880_rq1(device_id(), address_of(TEMP_RHYTHM, item * NOTE_LEN), NOTE_LEN, msg)
                        : jv880_rq1(device_id(), PART8_RX, 2, msg);
    midi_send_sysex(msg, n);
}

static void tick(int64_t now)
{
    if (send_next < KEYS) {
        if (now >= next_send_us) {
            send_bytes_to(send_base, send_next * NOTE_LEN, NOTE_LEN);
            send_next++;
            next_send_us = now + SEND_GAP_US + (int64_t)NOTE_LEN * BYTE_US;
        }
    } else if (read_item < READ_ITEMS && now >= next_send_us) {
        while (read_item < READ_ITEMS && (read_got >> read_item & 1)) {
            read_item++;  /* this one is in already */
        }
        if (read_item < READ_ITEMS) {
            request_item(read_item++);
            next_send_us = now + READ_GAP_US;
        }
        if (read_item >= READ_ITEMS) {
            read_check_at = now + READ_CHECK_US;
        }
    } else if (read_check_at && read_item >= READ_ITEMS && now >= read_check_at) {
        read_check_at = 0;
        if ((read_got & ALL_ITEMS) != ALL_ITEMS && read_tries < READ_TRIES) {
            read_tries++;
            read_item = 0;  /* the missing ones again */
        }
    }
}

static void read_kit(void)
{
    read_asked = true;
    as_read_ok = false;  /* the new read is what Rec + Erase goes back to */
    read_item = 0;
    read_got = 0;
    read_tries = 1;
    read_check_at = 0;
    send_next = KEYS;
    next_send_us = 0;
}

static void sysex_in(const uint8_t *d, size_t len)
{
    uint32_t addr;
    const uint8_t *data;
    int n = jv880_parse_dt1(d, len, device_id(), &addr, &data);
    if (n < 1) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    if (addr == PART8_RX && n >= 2) {
        part8_switch = data[0];
        part8_channel = data[1];
        part8_known = true;
        read_got |= 1ULL << KEYS;
        return;
    }
    int off = (int)(addr - TEMP_RHYTHM), k = off >> 7, w = off & 0x7F;
    if (off < 0 || k >= KEYS || w + n > NOTE_LEN) {
        return;
    }
    for (int i = 0; i < n; i++) {
        setup[k * NOTE_LEN + w + i] = data[i];
        known[k * NOTE_LEN + w + i] = true;
    }
    if (w == 0 && n == NOTE_LEN) {
        read_got |= 1ULL << k;
    }
    if (k == 0 && w == 0) {
        synth_state_received();  /* the kit itself arrives: no fallback */
        if (read_asked) {
            /* Auto (read) on purpose: the unit's kit replaces whatever library
             * kit was loaded */
            read_asked = false;
            synth_sound_replaced();
        }
    }
    if ((read_got & ((1ULL << KEYS) - 1)) == (1ULL << KEYS) - 1 && !as_read_ok) {
        memcpy(as_read, setup, SETUP_LEN);
        as_read_ok = true;
        synth_notice("KIT READ");
    }
    if (k == key) {
        synth_redraw();
    }
}

/* Rec + Erase on the unit's own kit: back to what it was when read */
static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(setup, as_read, SETUP_LEN);
    memset(known, 1, SETUP_LEN);
    send_kit(TEMP_RHYTHM);
    return true;
}

/* ---- editing ---- */

/* Stores v for the key (the parameter real) and sends it */
static void set_one(int real, int k, int v)
{
    int at = index_of(real, k), len = 1;
    if (params[real].wide) {
        setup[at] = v >> 4 & 0x0F;
        setup[at + 1] = v & 0x0F;
        known[at + 1] = true;
        len = 2;
    } else {
        setup[at] = v;
    }
    known[at] = true;
    send_bytes_to(TEMP_RHYTHM, at, len);
}

static int card_wave_count(int group) { return jv880_wave_count(group, card_number()); }
static bool wave_exists(int group, int number) { return jv880_wave_exists(group, number, card_number()); }

static bool param_step(int p, int steps)
{
    int k, real;
    if (!resolve(p, &k, &real)) {
        return false;
    }
    int v = value_or_init(real, k), nv = v + steps;
    int group = real == P_WAVE ? value_or_init(P_GROUP, k) : 0;
    int max = real == P_WAVE ? card_wave_count(group) - 1 : params[real].max;
    nv = nv < params[real].min ? params[real].min : nv > max ? max : nv;
    if (real == P_WAVE && !wave_exists(group, nv)) {
        /* a gap in the list: on to the next wave there is, in the way the
         * knob turns, or stay */
        int dir = steps > 0 ? 1 : -1, w = nv;
        while (w >= 0 && w <= max && !wave_exists(group, w)) {
            w += dir;
        }
        nv = w >= 0 && w <= max ? w : v;
    }
    if (nv == v && known[index_of(real, k)] && real != P_GROUP) {
        return false;
    }
    set_one(real, k, nv);
    if (real == P_GROUP) {
        /* another group: the wave goes to the nearest one it has, and is
         * sent too, so the sound changes at once */
        set_one(P_WAVE, k, jv880_wave_nearest(nv, value_or_init(P_WAVE, k), card_number()));
        synth_redraw();  /* the wave's column too */
    }
    return true;
}

static void param_toggle(int p)
{
    int k, real;
    if (resolve(p, &k, &real)) {
        set_one(real, k, known[index_of(real, k)] ? !get(setup, real, k) : 0);
    }
}

static bool param_reset(int p)
{
    int k, real;
    if (!resolve(p, &k, &real)) {
        return false;
    }
    read_init();
    int v = get(init_setup, real, k);
    if (known[index_of(real, k)] && get(setup, real, k) == v) {
        return false;
    }
    set_one(real, k, v);
    return true;
}

/* ---- formatting ---- */

static void note_name(int note, char *buf, size_t len)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[note % 12], note / 12 - 1);  /* 60 = C4 */
}

static const char *param_name(int p)
{
    static char kit_names[8][8];
    if (p >= P_KIT0) {
        char *n = kit_names[p - P_KIT0];
        int k, real;
        if (resolve(p, &k, &real)) {
            note_name(FIRST_KEY + k, n, sizeof(kit_names[0]));
        } else {
            snprintf(n, sizeof(kit_names[0]), "-");
        }
        return n;
    }
    return params[p].name;
}

static void param_format(int p, char *buf, size_t len)
{
    int k, real;
    if (!resolve(p, &k, &real)) {
        buf[0] = 0;
        return;
    }
    if (!known[index_of(real, k)]) {
        snprintf(buf, len, "--");
        return;
    }
    const jv_param_t *pp = &params[real];
    int v = get(setup, real, k);
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_PLUS1:
        snprintf(buf, len, "%d", v + 1);
        break;
    case K_LIST:
        snprintf(buf, len, "%s", pp->labels[v - pp->min <= pp->max - pp->min ? v - pp->min : 0]);
        break;
    case K_SWITCH:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    case K_PAN:
        if (v >= 128) {
            snprintf(buf, len, "RND");
        } else if (v < 64) {
            snprintf(buf, len, "L%d", 64 - v);
        } else {
            snprintf(buf, len, v == 64 ? "C" : "%dR", v - 64);
        }
        break;
    case K_NOTE:
        note_name(v, buf, len);
        break;
    case K_MUTE:
        if (v) {
            snprintf(buf, len, "%d", v);
        } else {
            snprintf(buf, len, "OFF");
        }
        break;
    case K_EG:
        if (pp->center) {
            snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        } else {
            snprintf(buf, len, "%d", v);
        }
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    int k, real;
    if (!resolve(p, &k, &real) || !known[index_of(real, k)]) {
        return -1;
    }
    const jv_param_t *pp = &params[real];
    if (pp->kind == K_EG || pp->kind == K_LIST || pp->kind == K_SWITCH || real == P_WAVE) {
        return -1;
    }
    return (get(setup, real, k) - pp->min) * 1000 / (pp->max - pp->min);
}

static bool param_on(int p)
{
    int k, real;
    return resolve(p, &k, &real) && known[index_of(real, k)] && get(setup, real, k);
}

static int param_units(int p)
{
    int k, real;
    if (!resolve(p, &k, &real)) {
        return 40;
    }
    int range = params[real].max - params[real].min;
    return range <= 1 ? 80 : range <= 4 ? 70 : range <= 16 ? 40 : range <= 31 ? 25
           : real == P_WAVE ? WAVE_UNITS : 10;  /* picking a wave among up to 255: slow */
}

/* ---- pictures ---- */

#define BAND_Y 14
#define BAND_H 38

/* Envelope columns: "T1 50" at the bottom, the envelope gets the rest */
static bool column_draw(int p, int disp, int x)
{
    if (p >= P_KIT0 || params[p].kind != K_EG) {
        return false;
    }
    char val[8];
    param_format(p, val, sizeof(val));
    jv880_draw_eg_text(disp, x, params[p].name, val);
    return true;
}

static int pv(int p) { return get(setup, p, key); }

static void page_draw(int pg, int disp)
{
    if (pg != PG_PENV && pg != PG_TVF_ENV && pg != PG_TVA_ENV) {
        return;
    }
    int first = pg == PG_PENV ? P_PT1 : pg == PG_TVF_ENV ? P_FT1 : P_AT1;
    if (!known[index_of(first, key)]) {
        display_fill_rect(disp, 0, BAND_Y, 256, BAND_H, false);
        return;
    }
    /* T1 L1 T2 L2 T3 L3 T4 (L4): the third level is held while the key is */
    int tm[5] = {0, pv(first), pv(first + 2), pv(first + 4), pv(first + 6)};
    int lv[5];
    if (pg == PG_PENV) {
        for (int i = 0; i < 4; i++) {
            lv[i + 1] = (pv(first + 1 + 2 * i) - 64) * 50 / 63 + 50;
        }
        lv[0] = 50;
        jv880_draw_env(disp, lv, tm, 5, 3, true);
    } else {
        for (int i = 0; i < 3; i++) {
            lv[i + 1] = pv(first + 1 + 2 * i) * 100 / 127;
        }
        lv[0] = 0;
        lv[4] = pg == PG_TVF_ENV ? pv(P_FL4) * 100 / 127 : 0;
        jv880_draw_env(disp, lv, tm, 5, 3, false);
    }
}

/* The wave number, and its name below when there is one; never a bar */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    int k, real;
    if (!resolve(p, &k, &real) || real != P_WAVE || !known[index_of(P_WAVE, k)]) {
        return SYNTH_DRAW_NONE;
    }
    char num[8];
    param_format(p, num, sizeof(num));
    jv880_draw_wave(disp, x, y, w, num, get(setup, P_GROUP, k), get(setup, P_WAVE, k), card_number());
    return SYNTH_DRAW_FULL;
}

/* ---- keys ---- */

static void select_key(int k)
{
    key = k < 0 ? 0 : k >= KEYS ? KEYS - 1 : k;
    synth_redraw();
}

/* Transport arrows: the next key in the pads' scale (chromatic by default) */
static void instance_step(int d)
{
    int note = synth_scale_note(FIRST_KEY + key, d);
    if (note >= FIRST_KEY && note < FIRST_KEY + KEYS) {
        select_key(note - FIRST_KEY);
    }
}

static void note_played(int note)
{
    if (note >= FIRST_KEY && note < FIRST_KEY + KEYS && note - FIRST_KEY != key) {
        select_key(note - FIRST_KEY);
    }
}

static int instance(void) { return 0; }  /* one kit for all keys */

/* The unit's setup has no name: say where it came from once it's read */
static bool sound_name(char *buf, size_t len)
{
    if (!known[0]) {
        return false;
    }
    snprintf(buf, len, "JV-880's kit");
    return true;
}

static void header_tag(char *buf, size_t len)
{
    char n[8];
    note_name(FIRST_KEY + key, n, sizeof(n));
    snprintf(buf, len, "KEY %s", n);
}

static int play_channel(void)
{
    return part8_known && part8_switch && part8_channel < 16 ? part8_channel : synth_setting(SETTING_CHANNEL);
}

/* The kit is played by the performance's part 8: Performance mode (System
 * Common, panel mode, manual p. 223) */
static bool activate(void)
{
    uint8_t mode = 0, msg[16];
    int n = jv880_dt1(device_id(), JV880_SYSTEM_COMMON, &mode, 1, msg);
    midi_send_sysex(msg, n);
    return true;
}

/* ---- kits ---- */

static bool patch_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(setup, 1, SETUP_LEN, f) == SETUP_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memset(known, 1, SETUP_LEN);
    as_read_ok = false;
    read_item = READ_ITEMS;  /* a read still going would bring the unit's old kit back */
    read_check_at = 0;
    send_kit(TEMP_RHYTHM);
    return true;
}

static bool patch_save(const char *path)
{
    read_init();
    for (int i = 0; i < SETUP_LEN; i++) {
        if (!known[i]) {
            setup[i] = init_setup[i];  /* never set: saved as the init kit's */
        }
    }
    memset(known, 1, SETUP_LEN);
    mkdir(STORAGE_ROOT "/jv880r", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(setup, 1, SETUP_LEN, f) == SETUP_LEN;
    fclose(f);
    return ok;
}

static int unknown_count(void)
{
    int n = 0;
    for (int i = 0; i < SETUP_LEN; i++) {
        n += !known[i];
    }
    return n;
}

/* ---- writing into the unit's own memory (Store: TO JV-880) ----
 * The unit has one internal Rhythm Setup (manual p. 223) */

static void unit_slot_name(int slot, char *buf, size_t len) { snprintf(buf, len, "INT RHYTHM"); }

static void unit_store(int slot, const char *name)
{
    read_init();
    for (int i = 0; i < SETUP_LEN; i++) {
        if (!known[i]) {
            setup[i] = init_setup[i];
        }
    }
    memset(known, 1, SETUP_LEN);
    send_kit(INTERNAL_RHYTHM);
    synth_notice("WRITING TO INT RHYTHM");
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"JV-880 RHYTHM", {"The Rhythm Setup: 61 keys C2-C7, each", "with its own complete tone. Hit a pad:",
                       "that key is edited. STEP arrows: the", "next key along the pads' scale.",
                       "A WAVE  B PITCH ENV  C TVF  D TVA", "E OUTPUT  F KIT: 8 keys' waves at once.",
                       "Second pages: envelopes."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_PAD_MODE, BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2,
      BTN_GROUP_A + 5, -1}},
    {"READ, WRITE", {"The unit plays the kit as part 8 of the", "performance: selecting this synth sets",
                     "Performance mode; part 8 needs its Rx", "switch on. AUTO reads the kit and part 8's",
                     "channel (MIDI OUT to the Maschine).", "REC > TO JV-880 (twice): write the kit", "into the internal Rhythm Setup."},
     {BTN_AUTO, BTN_REC, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_jv880r = {
    .name = "JV-880 RHYTHM",
    .device = "JV-880",
    .entry = "RHYTHM",
    .key = "jv880r",
    .patch_dir = STORAGE_ROOT "/jv880r",
    .default_patch = "000 Init",
    .first_user_patch = 1,
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
    .keep_state = true,  /* selecting it reads the unit's kit, sends nothing */
    .activate = activate,
    .activate_ms = 150,
    .volume_cc = 7,
    .volume_setting = SETTING_VOLUME_JV,
    .page_draw = page_draw,
    .column_draw = column_draw,
    .play_channel = play_channel,
    .instance_step = instance_step,
    .instance = instance,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_kit,
    .sysex_in = sysex_in,
    .tick = tick,
    .note_played = note_played,
    .unit_store_label = "TO JV-880",
    .unit_store_slots = 1,
    .unit_slot_name = unit_slot_name,
    .unit_store = unit_store,
    .theme_bright = {127, 90, 0},  /* amber, as JV-880 PATCH */
    .theme_dim = {14, 9, 0},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
