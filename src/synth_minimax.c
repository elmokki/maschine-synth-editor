/*
 * MiniMax (Minimoog model) running on the use-audio Plugiator.
 *
 * Every parameter is a Control Change or a Polyphonic Aftertouch message
 * with the note number used as a parameter id (minimax_params.c, generated
 * from the Plug-in Manager's config.xml). The Plugiator has no MIDI OUT, so
 * values are unknown until a patch is loaded or the parameter is touched.
 * Patch files are those same messages, 3 bytes each, channel nibble 0.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "display.h"
#include "midi.h"
#include "minimax_params.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "minimax";

#define MAX_PATCH_BYTES 512

/* Encoder counter units per value step, per parameter type: ~1300 units
 * per turn, so continuous values move ~130 per turn and 6-position knobs
 * one position per ~1/16 turn. */
#define UNITS_CONT 10
#define UNITS_STEP6 80
#define UNITS_RAW24 50

/* Main panel of the plug-in GUI first, in signal-chain order (OSC -> MIX
 * -> FILTER -> LOUDNESS), then its global controllers, then the Add panel's
 * extras. Every switch sits above the knob it belongs with (the Decay
 * switch shapes the envelope release, so it's above A DECAY rather than in
 * the controllers section where the panel has it), and unrelated
 * parameters don't share a page just to fill columns. Bypass affects all
 * effects, so it's on both effect pages. */
#define _ SYNTH_NONE
static const synth_page_t pages[] = {
    {0, "OSCILLATORS",
     {MINIMAX_OSC1_RNG, MINIMAX_OSC1_WAV, MINIMAX_OSC2_RNG, MINIMAX_OSC2_FRQ,
      MINIMAX_OSC2_WAV, MINIMAX_OSC3_RNG, MINIMAX_OSC3_FRQ, MINIMAX_OSC3_WAV},
     {MINIMAX_OSC_MOD, _, _, _, _, MINIMAX_OSC3_CTL, _, _}},
    {1, "MIXER",
     {MINIMAX_OSC1_VOL, MINIMAX_OSC2_VOL, MINIMAX_OSC3_VOL, MINIMAX_EXT_VOL,
      MINIMAX_NOISE_VOL, _, _, _},
     {MINIMAX_OSC1, MINIMAX_OSC2, MINIMAX_OSC3, MINIMAX_EXT,
      MINIMAX_NOISE, MINIMAX_PINK, MINIMAX_FEEDBACK, _}},
    {2, "FILTER",
     {MINIMAX_CUTOFF, MINIMAX_EMPHASIS, MINIMAX_CONTOUR, MINIMAX_F_ATTACK,
      MINIMAX_F_DECAY, MINIMAX_F_SUSTAIN, MINIMAX_F_VELO, _},
     {MINIMAX_FILT_MOD, MINIMAX_KBD_1, MINIMAX_KBD_2, _, _, _, _, _}},
    {3, "LOUDNESS",
     {MINIMAX_A_ATTACK, MINIMAX_A_DECAY, MINIMAX_A_SUSTAIN, MINIMAX_A_VELO,
      MINIMAX_VOLUME, _, _, _},
     {_, MINIMAX_DECAY_SW, _, _, MINIMAX_BOOST, _, _, _}},
    {4, "CONTROLLERS",
     {MINIMAX_TUNE, MINIMAX_GLIDE, MINIMAX_MOD_MIX, _, _, _, _, _},
     {_, MINIMAX_GLIDE_SW, _, _, _, _, _, _}},
    {5, "ADDITIONAL",
     {MINIMAX_BEND, MINIMAX_MW_INT, MINIMAX_MW_OFFS, MINIMAX_MOD_WHL,
      MINIMAX_CV_OSC, MINIMAX_CV_FILT, MINIMAX_CV_LOUD, _},
     {MINIMAX_LO_NOTE, MINIMAX_RETRIG, MINIMAX_SINGLE, _, _, _, _, _}},
    {6, "CHORUS",
     {MINIMAX_CH_RATE, MINIMAX_CH_DEPTH, MINIMAX_CH_PHASE, MINIMAX_CH_FDBK,
      MINIMAX_CH_MIX, _, _, _},
     {MINIMAX_FLANGER, _, _, _, _, _, _, MINIMAX_BYPASS}},
    {6, "DELAY",
     {MINIMAX_DLY_T_L, MINIMAX_DAMP_L, MINIMAX_LEVEL_L, MINIMAX_DLY_FDBK,
      MINIMAX_DLY_T_R, MINIMAX_DAMP_R, MINIMAX_LEVEL_R, MINIMAX_DLY_MIX},
     {_, _, _, MINIMAX_CROSS, _, _, _, MINIMAX_BYPASS}},
    {6, "DELAY SYNC",
     {MINIMAX_NOTE_L, MINIMAX_NOTE_R, MINIMAX_BPM, _, _, _, _, _},
     {MINIMAX_BPM_L, MINIMAX_BPM_R, _, _, _, _, _, _}},
};
#undef _

static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    [SETTING_PAD_NOTE] = {"PAD LOW", SET_NOTE, 0, 112, 36},  /* lowest pad (bottom left) = C1 */
};

static uint8_t values[MINIMAX_PARAM_COUNT];
static bool known[MINIMAX_PARAM_COUNT];
static bool initialised;

static const plug_param_t *prm(int p)
{
    return &minimax_params[p];
}

/* Values start at the factory defaults; nothing is known yet */
static void init_values(void)
{
    if (initialised) {
        return;
    }
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        values[p] = prm(p)->default_value;
    }
    initialised = true;
}

static int step6_pos(uint8_t v)
{
    int pos = (v + 11) / 22;
    return pos > 5 ? 5 : pos;
}

static void send_param(int p)
{
    uint8_t status = (prm(p)->msg == PLUG_CC ? 0xB0 : 0xA0) | synth_setting(SETTING_CHANNEL);
    midi_send(status, prm(p)->number, values[p]);
}

/* ---- parameters ---- */

static const char *param_name(int p)
{
    return prm(p)->short_name;
}

static void param_format(int p, char *buf, size_t len)
{
    uint8_t v = values[p];
    if (!known[p]) {
        snprintf(buf, len, "--");
    } else if (prm(p)->type == PT_SWITCH) {
        snprintf(buf, len, v >= 64 ? "ON" : "OFF");
    } else if (prm(p)->type == PT_STEP6) {
        if (prm(p)->labels) {
            snprintf(buf, len, "%s", prm(p)->labels[step6_pos(v)]);
        } else {
            snprintf(buf, len, "%d", step6_pos(v) + 1);
        }
    } else {
        snprintf(buf, len, "%d", v);
    }
}

static int param_bar(int p)
{
    if (!known[p] || prm(p)->type == PT_SWITCH) {
        return -1;
    }
    int max = prm(p)->type == PT_RAW24 ? 24 : prm(p)->type == PT_STEP6 ? 110 : 127;
    return values[p] * 1000 / max;
}

static bool param_on(int p)
{
    return known[p] && values[p] >= 64;
}

static int param_units(int p)
{
    return prm(p)->type == PT_STEP6 ? UNITS_STEP6 :
           prm(p)->type == PT_RAW24 ? UNITS_RAW24 : UNITS_CONT;
}

static bool param_step(int p, int steps)
{
    init_values();
    /* Unknown value: start from the factory default instead of guessing */
    int v = known[p] ? values[p] : prm(p)->default_value;
    if (prm(p)->type == PT_STEP6) {
        int pos = step6_pos(v) + steps;
        v = (pos < 0 ? 0 : pos > 5 ? 5 : pos) * 22;
    } else if (prm(p)->type == PT_SWITCH) {
        v = steps > 0 ? 127 : 0;
    } else {
        int max = prm(p)->type == PT_RAW24 ? 24 : 127;
        v += steps;
        v = v < 0 ? 0 : v > max ? max : v;
    }
    if (known[p] && v == values[p]) {
        return false;
    }
    values[p] = v;
    known[p] = true;
    send_param(p);
    return true;
}

static void param_toggle(int p)
{
    /* An unknown switch is drawn as off, so its first press turns it on -
     * toggling from the factory default could look like nothing happened */
    values[p] = known[p] && values[p] >= 64 ? 0 : 127;
    known[p] = true;
    send_param(p);
}

/* Two cycles of a Minimoog waveform (position 0-5 of the waveform knob) */
static void draw_wave(int disp, int x, int y, int w, int h, int pos)
{
    static const uint8_t pulse_duty[3] = {50, 30, 10};  /* SQUARE, WIDE, NARROW, % */
    int c = w / 2, top = y, bot = y + h - 1;
    for (int k = 0; k < 2; k++) {
        int x0 = x + k * c, x1 = x0 + c;
        switch (pos) {
        case 0:  /* triangle */
            display_line(disp, x0, bot, x0 + c / 2, top, true);
            display_line(disp, x0 + c / 2, top, x1, bot, true);
            break;
        case 1:  /* shark tooth: slow rise, quick fall */
            display_line(disp, x0, bot, x0 + c * 3 / 4, top, true);
            display_line(disp, x0 + c * 3 / 4, top, x1, bot, true);
            break;
        case 2:  /* sawtooth */
            display_line(disp, x0, bot, x1, top, true);
            display_line(disp, x1, top, x1, bot, true);
            break;
        default: {  /* square / wide / narrow pulse */
            int xh = x0 + c * pulse_duty[pos - 3] / 100;
            display_line(disp, x0, bot, x0, top, true);
            display_line(disp, x0, top, xh, top, true);
            display_line(disp, xh, top, xh, bot, true);
            display_line(disp, xh, bot, x1, bot, true);
            if (k == 1) {
                display_line(disp, x1, bot, x1, top, true);
            }
            break;
        }
        }
    }
}

static bool param_draw(int p, int disp, int x, int y, int w, int h)
{
    if (prm(p)->icon != PI_WAVE || !known[p]) {
        return false;
    }
    draw_wave(disp, x + 9, y, w - 18, h, step6_pos(values[p]));
    return true;
}

/* ---- patches ---- */

static int find_param(uint8_t status, uint8_t number)
{
    plug_msg_t msg = (status & 0xF0) == 0xB0 ? PLUG_CC : PLUG_PAT;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        if (prm(p)->msg == msg && prm(p)->number == number) {
            return p;
        }
    }
    return SYNTH_NONE;
}

static bool patch_load(const char *path)
{
    init_values();
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "can't open %s", path);
        return false;
    }
    uint8_t buf[MAX_PATCH_BYTES];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 3 <= n; i += 3) {
        uint8_t type = buf[i] & 0xF0;
        if (type != 0xB0 && type != 0xA0) {
            continue;
        }
        midi_send(type | synth_setting(SETTING_CHANNEL), buf[i + 1], buf[i + 2]);
        int p = find_param(type, buf[i + 1]);
        if (p != SYNTH_NONE) {
            values[p] = buf[i + 2];
            known[p] = true;
        }
    }
    return true;
}

static bool patch_save(const char *path)
{
    /* Same format as the imported presets: every in-preset parameter, in
     * table order; unknown ones as their factory default */
    uint8_t buf[MINIMAX_PARAM_COUNT * 3];
    size_t n = 0;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        if (!prm(p)->in_preset) {
            continue;
        }
        buf[n++] = prm(p)->msg == PLUG_CC ? 0xB0 : 0xA0;
        buf[n++] = prm(p)->number;
        buf[n++] = known[p] ? values[p] : prm(p)->default_value;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int unknown_count(void)
{
    int n = 0;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        n += prm(p)->in_preset && !known[p];
    }
    return n;
}

const synth_t synth_minimax = {
    .name = "MiniMax",
    .key = "minimax",
    .patch_dir = STORAGE_ROOT "/plugiator/minimax",
    .default_patch = "099 Init",
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
    .param_draw = param_draw,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .unknown_count = unknown_count,
};
