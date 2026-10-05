/*
 * Roland D-110, rhythm level: the unit's one Rhythm Setup - a drum kit,
 * each key C1-C8 (24-108) its own sound: a tone (one of the rhythm tones
 * r01-r64 or an internal tone i01-i64), level, pan and output - plus the
 * rhythm part's channel, partial reserve and level (part R).
 *
 * The key edited is the one last hit on the pads, or stepped to with the
 * transport arrows. A patch file is the setup: 85 keys x 4 bytes, as in the
 * Rhythm Setup Temporary Area (03 01 10, manual p. 118). Like the other
 * levels: nothing is sent when it's selected, the state is read back
 * (Auto) if the D-110's MIDI OUT is connected; values are "--" until then.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "d110_common.h"
#include "display.h"
#include "esp_log.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "buttons.h"
#include "synth.h"

static const char *TAG = "d110r";

#define MODEL_ID 0x16
#define FIRST_KEY 24
#define KEYS 85                     /* 24-108 */
#define SETUP_LEN (KEYS * 4)
#define RHYTHM_SETUP 0x030110       /* + (key - 24) * 4 */
#define RHYTHM_TIMBRE 0x030100      /* part R's timbre: output level at + 8 */
#define SYSTEM_AREA 0x100000        /* reserves 04-0C, channels 0D-15 */
#define SYS_RESERVE 0x04
#define SYS_CHANNEL 0x0D
#define PART_R 8

/* r01-r64 (manual p. 102); r64 = no sound */
static const char *const r_names[64] = {
    "Closed HH1", "Closed HH2", "Open HH 1", "Open HH 2", "Crash Cym", "Crash Shrt", "Crash Mute",
    "Ride Cym", "Ride Short", "Ride Mute", "Cup", "Cup Mute", "China Cym", "Splash Cym", "Bass Drum1",
    "Bass Drum2", "Bass Drum3", "Bass Drum4", "Snare 1", "Snare 2", "Snare 3", "Snare 4", "Snare 5",
    "Snare 6", "Rim Shot", "Brush 1", "Brush 2", "Hi Tom 1", "Mid Tom 1", "Low Tom 1", "Hi Tom 2",
    "Mid Tom 2", "Low Tom 2", "Hi Tom 3", "Mid Tom 3", "Low Tom 3", "HiPitchTm1", "HiPitchTm2",
    "Hand Clap", "Tambourine", "Cowbell", "Hi Bongo", "Low Bongo", "HiCongaMut", "Hi Conga",
    "Low Conga", "Hi Timbale", "LowTimbale", "Hi Agogo", "Low Agogo", "Cabasa", "Maracas",
    "Shrt Whstl", "Long Whstl", "Quijada", "Claves", "Castanets", "Triangle", "Wood Block", "Bell",
    "NativeDrm1", "NativeDrm2", "NativeDrm3", "OFF",
};

/* ---- parameters ---- */

enum {
    P_TONE, P_LEVEL, P_PAN, P_OUTPUT,           /* the selected key */
    P_KIT0, P_KIT7 = P_KIT0 + 7,                /* 8 keys' tones from it */
    P_CHANNEL, P_RESERVE, P_PART_LEVEL,         /* part R */
    PARAM_COUNT
};

static const char *const output_labels[] = {"MIX", "MIX", "MULTI 1", "MULTI 2", "MULTI 3",
                                            "MULTI 4", "MULTI 5", "MULTI 6"};

#define _ SYNTH_NONE
static const synth_page_t pages[] = {
    {0, "KEY", {P_TONE, P_LEVEL, P_PAN, P_OUTPUT, _, _, _, _}, {_, _, _, _, _, _, _, _}},
    /* 8 keys from the selected one, the way the pads go (Pad Mode scale):
     * build a kit quickly */
    {1, "KIT", {P_KIT0, P_KIT0 + 1, P_KIT0 + 2, P_KIT0 + 3, P_KIT0 + 4, P_KIT0 + 5, P_KIT0 + 6, P_KIT7},
     {_, _, _, _, _, _, _, _}},
    {2, "PART R", {P_CHANNEL, P_RESERVE, P_PART_LEVEL, _, _, _, _, _}, {_, _, _, _, _, _, _, _}},
};
#undef _

#define SETTING_UNIT 1
static const synth_setting_t settings[] = {
    /* Pads play the rhythm part on this until its channel is read */
    [SETTING_CHANNEL] = {"RHYTHM CH", SET_CHANNEL, 0, 15, 9},
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
};

/* ---- state ---- */

static uint8_t setup[SETUP_LEN];
static bool known[SETUP_LEN];
static uint8_t reserve[9], channel_r, level_r;
static bool reserve_known, channel_known, level_known;
static int key = 36 - FIRST_KEY;  /* C2, the bass drum */
static bool read_asked;

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }
static int addr7(int a3) { return (a3 >> 16 & 0x7F) * 16384 + (a3 >> 8 & 0x7F) * 128 + (a3 & 0x7F); }

static void dt1(int a, const uint8_t *data, int len)
{
    uint8_t msg[10 + SETUP_LEN];
    int n = 0, sum = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x41;
    msg[n++] = device_id();
    msg[n++] = MODEL_ID;
    msg[n++] = 0x12;
    msg[n++] = a >> 14 & 0x7F;
    msg[n++] = a >> 7 & 0x7F;
    msg[n++] = a & 0x7F;
    for (int i = 0; i < len; i++) {
        msg[n++] = data[i] & 0x7F;
    }
    for (int i = 5; i < n; i++) {
        sum += msg[i];
    }
    msg[n++] = (128 - (sum & 0x7F)) & 0x7F;
    msg[n++] = 0xF7;
    midi_send_sysex(msg, n);
}

static void rq1(int a, int size)
{
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x11, a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     size >> 14 & 0x7F, size >> 7 & 0x7F, size & 0x7F, 0, 0xF7};
    int sum = 0;
    for (int i = 5; i < 11; i++) {
        sum += msg[i];
    }
    msg[11] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
}

/* Setup bytes [from, from+len) to the unit; at most 128 bytes a message */
static void send_setup(int from, int len)
{
    for (int i = from; i < from + len; i += 128) {
        int n = from + len - i < 128 ? from + len - i : 128;
        dt1(addr7(RHYTHM_SETUP) + i, &setup[i], n);
    }
}

/* ---- parameter access ---- */

/* Key index (0-84) and byte within its 4 for p, or -1 */
static int byte_of(int p)
{
    if (p >= P_TONE && p <= P_OUTPUT) {
        return key * 4 + (p - P_TONE);
    }
    if (p >= P_KIT0 && p <= P_KIT7) {
        /* The keys the pads play: up the Pad Mode scale from the selected */
        int note = synth_scale_note(FIRST_KEY + key, p - P_KIT0);
        return note >= FIRST_KEY && note < FIRST_KEY + KEYS ? (note - FIRST_KEY) * 4 : -1;
    }
    return -1;
}

static bool value_known(int p)
{
    int b = byte_of(p);
    if (b >= 0) {
        return known[b];
    }
    return p == P_CHANNEL ? channel_known : p == P_RESERVE ? reserve_known : level_known;
}

static int value(int p)
{
    int b = byte_of(p);
    if (b >= 0) {
        return setup[b];
    }
    return p == P_CHANNEL ? channel_r : p == P_RESERVE ? reserve[PART_R] : level_r;
}

static int max_of(int p)
{
    switch (p) {
    case P_LEVEL: case P_PART_LEVEL: return 100;
    case P_PAN: return 14;
    case P_OUTPUT: return 7;
    case P_CHANNEL: return 16;
    case P_RESERVE: return 32;
    default: return 127;  /* tones */
    }
}

static bool is_tone(int p) { return p == P_TONE || (p >= P_KIT0 && p <= P_KIT7); }

static void note_name(int note, char *buf, size_t len)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[note % 12], note / 12 - 1);  /* 60 = C4, as the D-110 */
}

static const char *param_name(int p)
{
    static char kit_names[8][8];
    if (p >= P_KIT0 && p <= P_KIT7) {
        int b = byte_of(p);
        char *n = kit_names[p - P_KIT0];
        if (b >= 0) {
            note_name(FIRST_KEY + b / 4, n, sizeof(kit_names[0]));
        } else {
            snprintf(n, sizeof(kit_names[0]), "-");
        }
        return n;
    }
    static const char *const names[] = {"TONE", "LEVEL", "PAN", "OUTPUT"};
    if (p <= P_OUTPUT) {
        return names[p];
    }
    return p == P_CHANNEL ? "MIDI CH" : p == P_RESERVE ? "RESERVE" : "LEVEL";
}

/* i01-i64 / r01-r64 */
static void tone_code(int v, char *buf, size_t len)
{
    snprintf(buf, len, "%c%02d", v < 64 ? 'i' : 'r', v % 64 + 1);
}

static void param_format(int p, char *buf, size_t len)
{
    if (byte_of(p) < 0 && p >= P_KIT0 && p <= P_KIT7) {
        buf[0] = 0;
        return;
    }
    if (!value_known(p)) {
        snprintf(buf, len, "--");
        return;
    }
    int v = value(p);
    if (is_tone(p)) {
        tone_code(v, buf, len);
    } else if (p == P_PAN) {
        snprintf(buf, len, v - 7 ? "%+d" : "0", v - 7);
    } else if (p == P_OUTPUT) {
        snprintf(buf, len, "%s", output_labels[v & 7]);
    } else if (p == P_CHANNEL) {
        snprintf(buf, len, v >= 16 ? "OFF" : "%d", v + 1);
    } else {
        snprintf(buf, len, "%d", v);
    }
}

static int param_bar(int p)
{
    if (!value_known(p) || is_tone(p) || p == P_OUTPUT) {
        return -1;
    }
    return value(p) * 1000 / max_of(p);
}

static bool param_on(int p) { return false; }
static int param_units(int p) { return max_of(p) <= 16 ? 40 : max_of(p) <= 32 ? 25 : 16; }

static void send_reserves(void)
{
    /* The unit takes partial reserves only as all 9 parts together */
    dt1(addr7(SYSTEM_AREA) + SYS_RESERVE, reserve, 9);
}

static void set_value(int p, int v)
{
    int b = byte_of(p);
    if (b >= 0) {
        setup[b] = v;
        known[b] = true;
        send_setup(b, 1);
        return;
    }
    if (p == P_CHANNEL) {
        channel_r = v;
        channel_known = true;
        uint8_t c = v;
        dt1(addr7(SYSTEM_AREA) + SYS_CHANNEL + PART_R, &c, 1);
    } else if (p == P_RESERVE) {
        reserve[PART_R] = v;
        send_reserves();
    } else {
        level_r = v;
        level_known = true;
        uint8_t l = v;
        dt1(addr7(RHYTHM_TIMBRE) + 8, &l, 1);
    }
}

static bool param_step(int p, int steps)
{
    if (p >= P_KIT0 && p <= P_KIT7 && byte_of(p) < 0) {
        return false;
    }
    if (p == P_RESERVE && !reserve_known) {
        return false;  /* the others' reserves are needed to stay within 32 */
    }
    int max = max_of(p);
    if (p == P_RESERVE) {
        int others = 0;
        for (int i = 0; i < 8; i++) {
            others += reserve[i];
        }
        max = 32 - others < max ? 32 - others : max;
        max = max < 0 ? 0 : max;
    }
    int v = value_known(p) ? value(p) : 0, nv = v + steps;
    nv = nv < 0 ? 0 : nv > max ? max : nv;
    if (nv == v && value_known(p)) {
        return false;
    }
    set_value(p, nv);
    return true;
}

static void param_toggle(int p) {}
static bool param_reset(int p) { return false; }

/* Tones: code and name (r tones by name) */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    if (!is_tone(p) || byte_of(p) < 0 || !value_known(p)) {
        return SYNTH_DRAW_NONE;
    }
    char code[8], name[16];
    int v = value(p);
    tone_code(v, code, sizeof(code));
    snprintf(name, sizeof(name), "%s", v >= 64 ? r_names[v - 64] : "internal");
    display_text(disp, x + (w - display_text_width(&font_medium, code)) / 2, y + 1 - font_medium.cap_top,
                 &font_medium, code, true);
    display_text(disp, x + (w - display_text_width(&font_small, name)) / 2, y + 15 - font_small.cap_top,
                 &font_small, name, true);
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

static int instance(void) { return 0; }  /* one setup for all keys */

/* The unit's setup has no name: say where it came from once it's read */
static bool sound_name(char *buf, size_t len)
{
    if (!known[0]) {
        return false;
    }
    snprintf(buf, len, "D-110's kit");
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
    return channel_known && channel_r < 16 ? channel_r : synth_setting(SETTING_CHANNEL);
}

/* ---- reading back ---- */

static void read_state(void)
{
    read_asked = true;
    rq1(addr7(RHYTHM_SETUP), SETUP_LEN);
    rq1(addr7(SYSTEM_AREA), 0x16);   /* reserves and channels */
    rq1(addr7(RHYTHM_TIMBRE) + 8, 1);
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (len < 11 || d[1] != 0x41 || d[2] != device_id() || d[3] != MODEL_ID || d[4] != 0x12) {
        return;
    }
    int sum = 0;
    for (size_t i = 5; i < len - 1; i++) {
        sum += d[i];
    }
    if (sum & 0x7F) {
        return;
    }
    int a = d[5] << 14 | d[6] << 7 | d[7], n = (int)len - 10;
    bool got = false;
    for (int i = 0; i < n; i++) {
        int at = a + i, off;
        if ((off = at - addr7(RHYTHM_SETUP)) >= 0 && off < SETUP_LEN) {
            setup[off] = d[8 + i];
            known[off] = true;
            got = true;
        } else if ((off = at - addr7(SYSTEM_AREA)) >= SYS_RESERVE && off < SYS_RESERVE + 9) {
            reserve[off - SYS_RESERVE] = d[8 + i];
            reserve_known = true;
            got = true;
        } else if (off == SYS_CHANNEL + PART_R) {
            channel_r = d[8 + i];
            channel_known = true;
            got = true;
        } else if (at == addr7(RHYTHM_TIMBRE) + 8) {
            level_r = d[8 + i];
            level_known = true;
            got = true;
        }
    }
    if (got) {
        if (known[0]) {
            synth_state_received();  /* the kit itself arrived: no fallback */
        }
        if (read_asked) {
            read_asked = false;
            synth_sound_replaced();
        }
        synth_redraw();
    }
}

/* ---- patches (setups) ---- */

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
    memset(known, 1, sizeof(known));
    send_setup(0, SETUP_LEN);
    return true;
}

static bool patch_save(const char *path)
{
    mkdir(STORAGE_ROOT "/d110r", 0777);
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

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"DRUM KIT", {"Each key C1-C8 plays its own drum.", "Hit a pad: that key is edited.",
                  "STEP arrows: the next key, along the", "pads' scale (Pad Mode).",
                  "B KIT: 8 keys at once, one per knob.", "Setup's NOTES IN: notes coming in pick",
                  "keys too."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_GROUP_A + 1, BTN_PAD_MODE, -1, -1, -1, -1}},
    {"READ, VOLUME", {"AUTO: read the D-110's kit.", "The D-110 has one kit, no slots: save",
                      "your kits in the library (REC).", "Hold VOLUME + main encoder: Main Volume",
                      "on all the D-110's parts."},
     {BTN_AUTO, BTN_REC, BTN_VOLUME, -1, -1, -1, -1, -1}},
};

const synth_t synth_d110r = {
    .name = "D-110 RHYTHM",
    .key = "d110r",
    .patch_dir = STORAGE_ROOT "/d110r",
    .default_patch = "000 Factory",
    .first_user_patch = 1,
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
    .play_channel = play_channel,
    .instance_step = instance_step,
    .instance = instance,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_state,
    .sysex_in = sysex_in,
    .volume_get = d110_volume_get,
    .volume_set = d110_volume_set,
    .keep_state = true,
    .note_played = note_played,
    .theme_bright = {0, 100, 127},  /* cyan, as the other D-110 levels */
    .theme_dim = {0, 9, 12},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
