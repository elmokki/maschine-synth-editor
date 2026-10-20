/*
 * MiniMax (Minimoog model) on the use-audio Plugiator. The generic
 * CC/poly-AT parameter handling is in plugiator.c; this file has the page
 * layout, settings and the MiniMax's virtual parameters.
 */
#include <math.h>
#include <stdio.h>
#include "display.h"
#include "jv880_common.h"
#include "minimax_params.h"
#include "plugiator.h"
#include "storage.h"
#include "buttons.h"
#include "synth.h"

/* Main panel of the plug-in GUI first, in signal-chain order (OSC -> MIX
 * -> FILTER -> LOUDNESS, with the global controllers tune and glide), then
 * the Add panel's extras, then chorus and delay, each its own Group. Every switch sits above the knob it belongs with (the Decay
 * switch shapes the envelope release, so it's above A DECAY rather than in
 * the controllers where the panel has it), and unrelated
 * parameters don't share a page just to fill columns. MOD MIX (the
 * OSC3/noise balance of the modulation source) sits with the mixer, next
 * to those two sources. The mixer's 4th channel is the output fed back
 * pre-filter (GUI: Feedback Level), switched by the "External" switch; the
 * GUI's separate Feedback switch (C38) does nothing and isn't on a page.
 * FX ON (the GUI's "Bypass", but 127 = effects on) affects all effects,
 * so it's on both effect pages. */
/* Virtual parameters: shown and edited like any other, but stand for a
 * combination of real ones */
enum {
    /* Keyboard Control 1 (1/3 octave per octave) + 2 (2/3): cutoff key
     * tracking 0, 1/3, 2/3 or 1 octave per octave as one 4-position knob */
    VP_KEYTRACK = MINIMAX_PARAM_COUNT,
    /* Noise type as two buttons that switch each other off, like the
     * panel's white/pink rocker: both drive the one Noise Type CC */
    VP_NOISE_WHITE,
    VP_NOISE_PINK,
};
static const char *const keytrack_labels[4] = {"0", "1/3", "2/3", "1"};

#define _ SYNTH_NONE
static const synth_page_t pages[] = {
    {0, "OSCILLATORS",
     {MINIMAX_OSC1_RNG, MINIMAX_OSC1_WAV, MINIMAX_OSC2_RNG, MINIMAX_OSC2_FRQ,
      MINIMAX_OSC2_WAV, MINIMAX_OSC3_RNG, MINIMAX_OSC3_FRQ, MINIMAX_OSC3_WAV},
     {MINIMAX_OSC_MOD, _, _, _, _, MINIMAX_OSC3_CTL, _, _}},
    {1, "MIXER",
     {MINIMAX_OSC1_VOL, MINIMAX_OSC2_VOL, MINIMAX_OSC3_VOL, MINIMAX_FDBK_LVL,
      MINIMAX_NOISE_VOL, MINIMAX_MOD_MIX, _, _},
     {MINIMAX_OSC1, MINIMAX_OSC2, MINIMAX_OSC3, MINIMAX_FEEDBACK,
      MINIMAX_NOISE, _, VP_NOISE_WHITE, VP_NOISE_PINK}},
    {2, "FILTER",
     {MINIMAX_CUTOFF, MINIMAX_EMPHASIS, MINIMAX_CONTOUR, VP_KEYTRACK,
      MINIMAX_F_ATTACK, MINIMAX_F_DECAY, MINIMAX_F_SUSTAIN, MINIMAX_F_VELO},
     {MINIMAX_FILT_MOD, _, _, _, _, _, _, _}},
    /* The panel's controllers (tune, glide) share the page with the
     * loudness: glide's switch sits above it */
    {3, "LOUDNESS",
     {MINIMAX_A_ATTACK, MINIMAX_A_DECAY, MINIMAX_A_SUSTAIN, MINIMAX_A_VELO,
      MINIMAX_VOLUME, MINIMAX_TUNE, MINIMAX_GLIDE, _},
     {_, MINIMAX_DECAY_SW, _, _, MINIMAX_BOOST, _, MINIMAX_GLIDE_SW, _}},
    {4, "ADDITIONAL",
     {MINIMAX_BEND, MINIMAX_MW_INT, MINIMAX_MW_OFFS, MINIMAX_MOD_WHL,
      MINIMAX_CV_OSC, MINIMAX_CV_FILT, MINIMAX_CV_LOUD, _},
     {MINIMAX_LO_NOTE, MINIMAX_RETRIG, MINIMAX_SINGLE, _, _, _, _, _}},
    {5, "CHORUS",
     {MINIMAX_CH_RATE, MINIMAX_CH_DEPTH, MINIMAX_CH_PHASE, MINIMAX_CH_FDBK,
      MINIMAX_CH_MIX, _, _, _},
     {MINIMAX_FLANGER, _, _, _, _, _, _, MINIMAX_FX_ON}},
    {6, "DELAY",
     {MINIMAX_DLY_T_L, MINIMAX_DAMP_L, MINIMAX_LEVEL_L, MINIMAX_DLY_FDBK,
      MINIMAX_DLY_T_R, MINIMAX_DAMP_R, MINIMAX_LEVEL_R, MINIMAX_DLY_MIX},
     {_, _, _, MINIMAX_CROSS, _, _, _, MINIMAX_FX_ON}},
    {6, "DELAY SYNC",
     {MINIMAX_NOTE_L, MINIMAX_NOTE_R, MINIMAX_BPM, _, _, _, _, _},
     {MINIMAX_BPM_L, MINIMAX_BPM_R, _, _, _, _, _, _}},
};
#undef _

/* Sent after the plug-in loads, see synth_t.device_ccs */
static const synth_device_cc_t device_ccs[] = {
    {SETTING_SYNTH_MIC, 93},
    {0, 0},
};

static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    [SETTING_PLUGIN_PC] = {"PLUGIN PC", SET_NUMBER, 0, 127, 101},
    [SETTING_VOLUME] = {"VOLUME", SET_NUMBER, 0, 127, 100},  /* Plugiator device volume */
    /* Synth/mic input balance (CC93, not part of presets): 0 = synth only */
    [SETTING_SYNTH_MIC] = {"SYNTH/MIC", SET_NUMBER, 0, 127, 0},
};

static uint8_t values[MINIMAX_PARAM_COUNT], init_values[MINIMAX_PARAM_COUNT];
static bool known[MINIMAX_PARAM_COUNT];

static plug_set_t set = {
    .tag = "minimax",
    .params = minimax_params,
    .count = MINIMAX_PARAM_COUNT,
    .values = values,
    .known = known,
    .init = init_values,
    .patch_dir = STORAGE_ROOT "/plugiator/minimax",
    .default_patch = "099 Init",
};

static bool is_virtual(int p)
{
    return p >= MINIMAX_PARAM_COUNT;
}

/* 0-3, or -1 while either switch is unknown */
static int keytrack_pos(void)
{
    if (!known[MINIMAX_KBD_1] || !known[MINIMAX_KBD_2]) {
        return -1;
    }
    return (values[MINIMAX_KBD_1] >= 64) + 2 * (values[MINIMAX_KBD_2] >= 64);
}

static const char *param_name(int p)
{
    switch (p) {
    case VP_KEYTRACK: return "KEYTRACK";
    case VP_NOISE_WHITE: return "WHITE";
    case VP_NOISE_PINK: return "PINK";
    default: return plug_name(&set, p);
    }
}

static void param_format(int p, char *buf, size_t len)
{
    if (p == VP_KEYTRACK) {
        int pos = keytrack_pos();
        snprintf(buf, len, "%s", pos < 0 ? "--" : keytrack_labels[pos]);
    } else if (!is_virtual(p)) {
        plug_format(&set, p, buf, len);
    } else {
        buf[0] = '\0';
    }
}

static int param_bar(int p)
{
    if (p == VP_KEYTRACK) {
        int pos = keytrack_pos();
        return pos < 0 ? -1 : pos * 1000 / 3;
    }
    return is_virtual(p) ? -1 : plug_bar(&set, p);
}

static bool param_on(int p)
{
    if (p == VP_NOISE_WHITE || p == VP_NOISE_PINK) {
        bool pink = values[MINIMAX_PINK] >= 64;
        return known[MINIMAX_PINK] && (p == VP_NOISE_PINK) == pink;
    }
    return is_virtual(p) ? false : plug_on(&set, p);
}

static int param_units(int p)
{
    /* KEYTRACK moves like the stepped knobs */
    return plug_units(&set, p == VP_KEYTRACK ? MINIMAX_OSC1_WAV : p);
}

static bool param_step(int p, int steps)
{
    if (p == VP_KEYTRACK) {
        plug_init_values(&set);
        int pos = keytrack_pos();
        int next = (pos < 0 ? 0 : pos) + steps;
        next = next < 0 ? 0 : next > 3 ? 3 : next;
        if (next == pos) {
            return false;
        }
        plug_set(&set, MINIMAX_KBD_1, next & 1 ? 127 : 0);
        plug_set(&set, MINIMAX_KBD_2, next & 2 ? 127 : 0);
        return true;
    }
    return is_virtual(p) ? false : plug_step(&set, p, steps);
}

static void param_toggle(int p)
{
    if (p == VP_NOISE_WHITE || p == VP_NOISE_PINK) {
        /* Selects its own position; pressing the lit one changes nothing */
        plug_set(&set, MINIMAX_PINK, p == VP_NOISE_PINK ? 127 : 0);
    } else if (!is_virtual(p)) {
        plug_toggle(&set, p);
    }
}

static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    return is_virtual(p) ? SYNTH_DRAW_NONE : plug_draw(&set, p, disp, x, y, w, h);
}

static bool param_reset(int p)
{
    switch (p) {
    case VP_KEYTRACK: {
        bool a = plug_reset(&set, MINIMAX_KBD_1);
        bool b = plug_reset(&set, MINIMAX_KBD_2);
        return a || b;
    }
    case VP_NOISE_WHITE:
    case VP_NOISE_PINK:
        return plug_reset(&set, MINIMAX_PINK);
    default:
        return plug_reset(&set, p);
    }
}

static bool patch_load(const char *path)
{
    return plug_patch_load(&set, path);
}

static bool patch_save(const char *path)
{
    return plug_patch_save(&set, path);
}

static int unknown_count(void)
{
    return plug_unknown_count(&set);
}

static bool activate(void)
{
    return plug_activate(&set);
}

static bool is_loaded(void)
{
    return plug_is_loaded(&set);
}

/* ---- patch overview (Grid): oscillators and mixer on the left display, the two envelopes on the right ---- */

static int val(int p) { return known[p] ? values[p] : minimax_params[p].default_value; }
static bool sw_on(int p) { return val(p) >= 64; }

/* An envelope time knob as the picture's 0-127 width: logarithmic in milliseconds */
static int time_width(int p)
{
    float t = log2f(1.0f + plug_time_ms(&minimax_params[p], val(p)) / 10.0f) * 12.0f;
    return t > 127.0f ? 127 : (int)t;
}

static void ov_text(int disp, int x, int y, const char *text)
{
    display_text(disp, x, y, &font_tiny, text, true);
}

static void ov_fmt(int p, char *buf, size_t len) { plug_format(&set, p, buf, len); }

/* One envelope: attack to the top, decay to the sustain level, held, then the release (the Decay switch makes
 * the decay time the release too) */
static void ov_envelope(int disp, int x, const char *title, int a, int d, int s_, int sw_decay)
{
    char buf[40], ta[12], td[12];
    ov_text(disp, x + 2, 13, title);
    int sus = val(s_) * 100 / 127;
    int lv[4] = {0, 100, sus, 0}, tm[4] = {0, time_width(a), time_width(d), sw_on(sw_decay) ? time_width(d) : 20};
    jv880_draw_env_mini(disp, x + 3, 22, 120, 26, lv, tm, 4, 2);
    ov_fmt(a, ta, sizeof(ta));
    ov_fmt(d, td, sizeof(td));
    snprintf(buf, sizeof(buf), "A %s D %s S %d%%", ta, td, sus);
    ov_text(disp, x + 2, 50, buf);
}

static void overview_draw(int page, int disp)
{
    (void)page;
    char buf[40];
    if (disp == 0) {
        display_text(0, 1, 0, &font_small, "MINIMAX: oscillators, mixer", true);
        static const int8_t wav[3] = {MINIMAX_OSC1_WAV, MINIMAX_OSC2_WAV, MINIMAX_OSC3_WAV};
        static const int8_t rng[3] = {MINIMAX_OSC1_RNG, MINIMAX_OSC2_RNG, MINIMAX_OSC3_RNG};
        static const int8_t vol[3] = {MINIMAX_OSC1_VOL, MINIMAX_OSC2_VOL, MINIMAX_OSC3_VOL};
        static const int8_t sw[3] = {MINIMAX_OSC1, MINIMAX_OSC2, MINIMAX_OSC3};
        static const int8_t frq[3] = {-1, MINIMAX_OSC2_FRQ, MINIMAX_OSC3_FRQ};
        for (int i = 0; i < 4; i++) {
            int x = i * 64;
            int level_p = i < 3 ? vol[i] : MINIMAX_NOISE_VOL, on = sw_on(i < 3 ? sw[i] : MINIMAX_NOISE);
            if (i < 3) {
                snprintf(buf, sizeof(buf), on ? "OSC%d %s" : "OSC%d off", i + 1,
                         minimax_params[rng[i]].labels[plug_step_pos(&minimax_params[rng[i]], val(rng[i]))]);
            } else {
                snprintf(buf, sizeof(buf), on ? "NOISE" : "NOISE off");
            }
            ov_text(0, x + 2, 13, buf);
            if (on && i < 3) {
                int pos = plug_step_pos(&minimax_params[wav[i]], val(wav[i]));
                plug_draw_wave(0, x + 8, 23, 48, 14, pos);
                ov_text(0, x + 2, 40, minimax_params[wav[i]].labels[pos]);
                if (frq[i] >= 0) {
                    char f[12];
                    ov_fmt(frq[i], f, sizeof(f));
                    snprintf(buf, sizeof(buf), "FRQ %s", f);
                    ov_text(0, x + 2, 48, buf);
                }
            } else if (on) {
                ov_text(0, x + 2, 40, sw_on(MINIMAX_PINK) ? "PINK" : "WHITE");
            }
            if (on) {  /* the level */
                display_rect(0, x + 2, 57, 58, 6, true);
                display_fill_rect(0, x + 4, 59, 54 * val(level_p) / 127, 2, true);
            }
        }
    } else {
        display_text(1, 1, 0, &font_small, "ENVELOPES, filter", true);
        display_text(1, 200, 0, &font_small, "Grid: close", true);
        ov_envelope(1, 0, "FILTER ENVELOPE", MINIMAX_F_ATTACK, MINIMAX_F_DECAY, MINIMAX_F_SUSTAIN, MINIMAX_DECAY_SW);
        ov_envelope(1, 128, "LOUDNESS ENVELOPE", MINIMAX_A_ATTACK, MINIMAX_A_DECAY, MINIMAX_A_SUSTAIN,
                    MINIMAX_DECAY_SW);
        char c[12], e[12], k[12];
        ov_fmt(MINIMAX_CUTOFF, c, sizeof(c));
        ov_fmt(MINIMAX_EMPHASIS, e, sizeof(e));
        ov_fmt(MINIMAX_CONTOUR, k, sizeof(k));
        snprintf(buf, sizeof(buf), "CUT %s EMP %s CNT %s", c, e, k);
        ov_text(1, 2, 57, buf);
        snprintf(buf, sizeof(buf), "VOL %d  VEL F%d A%d", val(MINIMAX_VOLUME), val(MINIMAX_F_VELO), val(MINIMAX_A_VELO));
        ov_text(1, 130, 57, buf);
    }
    display_fill_rect(disp, 0, 11, 255, 1, true);
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"PLUGIATOR", {"Picking MiniMax loads the plug-in on", "the Plugiator (about a second). It stays",
                   "loaded: coming back is instant (LOADED", "in the synth list, STEP).",
                   "The Plugiator can't send its sound back:", "values show -- until a patch is loaded",
                   "or the knob is turned."},
     {BTN_STEP, -1, -1, -1, -1, -1, -1, -1}},
    {"VOLUME", {"Hold VOLUME and turn the main encoder:", "the Plugiator's own output volume.", "",
                "Which Program Change loads the plug-in", "(PLUGIN PC) and SYNTH/MIC are in Setup",
                "(CONTROL)."},
     {BTN_VOLUME, BTN_CONTROL, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_minimax = {
    .name = "Plugiator MiniMax",
    .device = "Plugiator",
    .entry = "MiniMax",
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
    .param_reset = param_reset,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .unknown_count = unknown_count,
    .activate = activate,
    .is_loaded = is_loaded,
    .activate_ms = 1500,
    .volume_cc = 7,  /* "Plugiator Device Volume", not part of presets */
    .volume_setting = SETTING_VOLUME,
    .device_ccs = device_ccs,
    .categories = minimax_categories,
    .theme_bright = {127, 35, 0},  /* red-orange, like the Minimoog's orange switches */
    .theme_dim = {20, 4, 0},
    .overview_pages = 1,
    .overview_draw = overview_draw,
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
