/*
 * B4000 (Hammond B3 with Leslie model) on the use-audio Plugiator. The
 * generic CC/poly-AT parameter handling is in plugiator.c; this file has
 * the page layout and settings.
 */
#include "b4000_params.h"
#include "plugiator.h"
#include "storage.h"
#include "synth.h"

/* The organ's three parts first: Upper and Lower manual are parallel
 * (parallel = 1) two-page sections (9 drawbars don't fit 8 encoders: page 1 = 16'-1 1/3',
 * page 2 = 1' and the manual's own settings, so Upper 2 <-> Lower 2 line
 * up), then the pedal, then the panel's sound sections, the rotary
 * speaker (horn | bass), the rest of the organ model, and the effects
 * (same as every plug-in). Every switch sits above the knob it belongs
 * with. */
#define _ SYNTH_NONE
static const synth_page_t pages[] = {
    {0, "UPPER",
     {B4000_UPPER_16, B4000_UPPER_5_1_3, B4000_UPPER_8, B4000_UPPER_4,
      B4000_UPPER_2_2_3, B4000_UPPER_2, B4000_UPPER_1_3_5, B4000_UPPER_1_1_3},
     {_, _, _, _, _, _, _, _}, 1},
    {0, "UPPER",
     {B4000_UPPER_1, B4000_UPPER_VOL, B4000_UPP_OCT, B4000_UPP_SPLIT,
      B4000_VIB_TYPE, _, _, _},
     {_, _, _, _, B4000_VIB_UPPER, _, _, _}, 1},
    {1, "LOWER",
     {B4000_LOWER_16, B4000_LOWER_5_1_3, B4000_LOWER_8, B4000_LOWER_4,
      B4000_LOWER_2_2_3, B4000_LOWER_2, B4000_LOWER_1_3_5, B4000_LOWER_1_1_3},
     {_, _, _, _, _, _, _, _}, 1},
    {1, "LOWER",
     {B4000_LOWER_1, B4000_LOWER_VOL, B4000_LOW_OCT, B4000_LOW_SPLIT,
      B4000_VIB_TYPE, _, _, _},
     {_, _, _, _, B4000_VIB_LOWER, _, _, _}, 1},
    {2, "PEDAL",
     {B4000_PEDAL_16, B4000_PEDAL_5_1_3, B4000_PEDAL_8, B4000_PEDAL_4,
      B4000_PEDAL_2_2_3, B4000_PEDAL_2, B4000_PEDAL_VOL, B4000_PED_OCT},
     {_, _, _, _, _, _, _, _}},
    {3, "VIBRATO / PERC",
     {B4000_VIB_TYPE, B4000_PERC_LVL, B4000_PERC_DEC, B4000_PERC_HARM,
      B4000_KEY_CLICK, B4000_SWELL, B4000_TREBLE, B4000_BASS},
     {_, B4000_PERC, _, _, _, _, _, _}},
    {4, "ROTOR HORN",
     {B4000_H_SLOW, B4000_H_FAST, B4000_H_ACCEL, B4000_H_BRAKE,
      B4000_H_TONE, B4000_SPREAD, B4000_BALANCE, B4000_DISTANCE},
     {B4000_ROTOR, B4000_FAST, B4000_AT, _, _, _, _, _}},
    {4, "ROTOR BASS",
     {B4000_B_SLOW, B4000_B_FAST, B4000_B_ACCEL, B4000_B_BRAKE,
      B4000_B_TONE, B4000_EXT_LVL, _, _},
     {B4000_ROTOR, B4000_FAST, B4000_AT, _, _, B4000_EXT_IN, _, _}},
    {5, "ORGAN",
     {B4000_DRIVE_LVL, B4000_OUTPUT, B4000_DISTORT, B4000_LEAKAGE,
      B4000_CONDITION, B4000_TUNING, B4000_ATTACK, B4000_RELEASE},
     {B4000_DRIVE, B4000_BOOST, _, _, _, _, _, B4000_VELOCITY}},
    {6, "CHORUS",
     {B4000_CH_RATE, B4000_CH_DEPTH, B4000_CH_PHASE, B4000_CH_FDBK,
      B4000_CH_MIX, _, _, _},
     {B4000_FLANGER, _, _, _, _, _, _, B4000_FX_ON}},
    {6, "DELAY",
     {B4000_DLY_T_L, B4000_DAMP_L, B4000_LEVEL_L, B4000_DLY_FDBK,
      B4000_DLY_T_R, B4000_DAMP_R, B4000_LEVEL_R, B4000_DLY_MIX},
     {_, _, _, B4000_CROSS, _, _, _, B4000_FX_ON}},
    {6, "DELAY SYNC",
     {B4000_NOTE_L, B4000_NOTE_R, B4000_BPM, _, _, _, _, _},
     {B4000_BPM_L, B4000_BPM_R, _, _, _, _, _, _}},
};
#undef _

/* Sent after the plug-in loads, see synth_t.device_ccs */
static const synth_device_cc_t device_ccs[] = {
    {SETTING_SYNTH_MIC, 93},
    {0, 0},
};

static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    [SETTING_PLUGIN_PC] = {"PLUGIN PC", SET_NUMBER, 0, 127, 102},
    [SETTING_VOLUME] = {"VOLUME", SET_NUMBER, 0, 127, 100},  /* Plugiator device volume */
    /* Synth/mic input balance (CC93, not part of presets): 0 = synth only */
    [SETTING_SYNTH_MIC] = {"SYNTH/MIC", SET_NUMBER, 0, 127, 0},
};

static uint8_t values[B4000_PARAM_COUNT], init_values[B4000_PARAM_COUNT];
static bool known[B4000_PARAM_COUNT];

static plug_set_t set = {
    .tag = "b4000",
    .params = b4000_params,
    .count = B4000_PARAM_COUNT,
    .values = values,
    .known = known,
    .init = init_values,
    .patch_dir = STORAGE_ROOT "/plugiator/b4000",
    /* The factory bank has no init patch; this is its plainest sound */
    .default_patch = "024 Very Basic",
};

static const char *param_name(int p) { return plug_name(&set, p); }
static void param_format(int p, char *buf, size_t len) { plug_format(&set, p, buf, len); }
static int param_bar(int p) { return plug_bar(&set, p); }
static bool param_on(int p) { return plug_on(&set, p); }
static int param_units(int p) { return plug_units(&set, p); }
static bool param_step(int p, int steps) { return plug_step(&set, p, steps); }
static void param_toggle(int p) { plug_toggle(&set, p); }
static bool param_reset(int p) { return plug_reset(&set, p); }
static bool patch_load(const char *path) { return plug_patch_load(&set, path); }
static bool patch_save(const char *path) { return plug_patch_save(&set, path); }
static int unknown_count(void) { return plug_unknown_count(&set); }
static void activate(void) { plug_activate(&set); }

static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    return plug_draw(&set, p, disp, x, y, w, h);
}

const synth_t synth_b4000 = {
    .name = "B4000",
    .key = "b4000",
    .patch_dir = STORAGE_ROOT "/plugiator/b4000",
    .default_patch = "024 Very Basic",
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
    .activate_ms = 1500,
    .volume_cc = 7,  /* "Plugiator Device Volume", not part of presets */
    .volume_setting = SETTING_VOLUME,
    .device_ccs = device_ccs,
    .categories = b4000_categories,
    .theme_bright = {90, 0, 127},  /* violet */
    .theme_dim = {9, 0, 14},
};
