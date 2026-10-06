/*
 * Roland JV-880, Patch mode: a patch = common part + four tones. Group A-H
 * are function blocks (A wave, B common, C pitch, D TVF, E TVA with the
 * output sends, F LFO 1, G LFO 2, H controllers), the transport Step arrows and Shift + Group
 * A-D pick the tones being edited (several at once, like the D-110's
 * partials); each block's seldom used parameters are on its last pages.
 *
 * Every change goes out at once as a DT1 to the Patch Mode Temporary Patch
 * (manual p. 223, 226-227). Reading it back (Auto) is optional and needs the
 * JV-880's MIDI OUT on the Maschine's MIDI IN: values are "--" until a patch
 * is loaded, read or the parameter is changed.
 *
 * A patch file is a patch: its 498 bytes (34 common + 4 x 116 tone) in the
 * order of the temporary patch's blocks.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "jv880";

#define COMMON_LEN JV880_PATCH_COMMON_LEN
#define TONE_LEN JV880_PATCH_TONE_LEN
#define PATCH_LEN JV880_PATCH_LEN
#define BLOCKS 5  /* block 0 = common, 1-4 = tones */
#define WAVE_UNITS 13  /* encoder counter units per wave number step (others 10-80); fast turns accelerate */
#define TONE_GAP 128  /* tones sit 01 00 apart: 128 addresses */

/* ---- parameters ---- */

typedef enum {
    K_NUM,       /* as is */
    K_SIGNED,    /* value - center */
    K_SIGNED10,  /* (value - center) * 10: LFO pitch depth in cents */
    K_PLUS1,     /* stored 0.., shown 1.. */
    K_LIST,      /* labels[value - min] */
    K_SWITCH,
    K_PAN,       /* 0-127 = L64..63R, 128 = RND */
    K_DELAY,     /* 0-127, 128 = KEY-OFF */
    K_EG,        /* envelope time/level: value only, the envelope is drawn */
} kind_t;

typedef struct {
    const char *name;  /* fits a 64 px display column */
    uint8_t off;       /* in the common part, or in each tone */
    kind_t kind;
    int16_t min, max, center;
    const char *const *labels;
    bool tone;         /* in each tone, not the common part */
    bool wide;         /* two nibble bytes (off = high, off + 1 = low) */
} jv_param_t;

static const char *const group_labels[] = {"INT", "EXP", "PCM"};
static const char *const reverb_labels[] = {"ROOM1", "ROOM2", "STAGE1", "STAGE2", "HALL1", "HALL2", "DELAY", "PAN-DLY"};
static const char *const chorus_labels[] = {"CHORUS1", "CHORUS2", "CHORUS3"};
static const char *const chorus_out_labels[] = {"MIX", "REV"};
static const char *const key_assign_labels[] = {"POLY", "SOLO"};
static const char *const porta_mode_labels[] = {"LEGATO", "NORMAL"};
static const char *const porta_type_labels[] = {"TIME", "RATE"};
static const char *const lfo_form_labels[] = {"TRI", "SIN", "SAW", "SQR", "RND1", "RND2"};
static const char *const lfo_offset_labels[] = {"-100", "-50", "0", "+50", "+100"};
static const char *const fade_labels[] = {"IN", "OUT"};
static const char *const dest_labels[] = {"OFF", "PITCH", "CUTOFF", "RESO", "LEVEL", "P-LFO1", "P-LFO2",
                                          "F-LFO1", "F-LFO2", "A-LFO1", "A-LFO2", "L1 RATE", "L2 RATE"};
static const char *const random_labels[] = {"0", "5", "10", "20", "30", "40", "50", "70",
                                            "100", "200", "300", "400", "500", "600", "800", "1200"};
static const char *const keyf_labels[] = {"-100", "-70", "-50", "-30", "-10", "0", "+10", "+20",
                                          "+30", "+40", "+50", "+70", "+100", "+120", "+150", "+200"};
static const char *const sense_labels[] = {"-100", "-70", "-50", "-40", "-30", "-20", "-10", "0",
                                           "+10", "+20", "+30", "+40", "+50", "+70", "+100"};
static const char *const filter_labels[] = {"OFF", "LPF", "HPF"};
static const char *const reso_labels[] = {"SOFT", "HARD"};
static const char *const dmode_labels[] = {"NORMAL", "HOLD", "PLAYMATE"};
static const char *const output_labels[] = {"MAIN", "SUB"};

enum {
    /* common */
    P_VELO_SW, P_REV_TYPE, P_REV_LEVEL, P_REV_TIME, P_DELAY_FB, P_CHO_TYPE, P_CHO_LEVEL, P_CHO_DEPTH,
    P_CHO_RATE, P_CHO_FB, P_CHO_OUT, P_ANALOG, P_LEVEL, P_PAN, P_BEND_DN, P_BEND_UP, P_KEY_ASSIGN,
    P_SOLO_LEG, P_PORTA_SW, P_PORTA_MODE, P_PORTA_TYPE, P_PORTA_TIME,
    COMMON_PARAMS,
    /* tone */
    P_GROUP = COMMON_PARAMS, P_WAVE, P_TONE_SW, P_FXM_SW, P_FXM_DEPTH, P_VEL_LO, P_VEL_HI, P_VOL_SW,
    P_HOLD_SW,
    P_MOD,                         /* 4 x (destination, sense) */
    P_AFT = P_MOD + 8,
    P_EXPR = P_AFT + 8,
    P_L1_FORM = P_EXPR + 8, P_L1_OFFSET, P_L1_SYNC, P_L1_RATE, P_L1_DELAY, P_L1_FADE_POL, P_L1_FADE_TIME,
    P_L1_PITCH, P_L1_TVF, P_L1_TVA,
    P_L2_FORM, P_L2_OFFSET, P_L2_SYNC, P_L2_RATE, P_L2_DELAY, P_L2_FADE_POL, P_L2_FADE_TIME, P_L2_PITCH,
    P_L2_TVF, P_L2_TVA,
    P_COARSE, P_FINE, P_RANDOM, P_PKF, P_PE_VELO, P_PE_ON, P_PE_OFF, P_PE_TKF, P_PE_DEPTH,
    P_PT1, P_PL1, P_PT2, P_PL2, P_PT3, P_PL3, P_PT4, P_PL4,
    P_F_MODE, P_CUTOFF, P_RESO, P_RESO_MODE, P_FKF, P_F_CURVE, P_F_VELO, P_F_ON, P_F_OFF, P_F_TKF, P_F_DEPTH,
    P_FT1, P_FL1, P_FT2, P_FL2, P_FT3, P_FL3, P_FT4, P_FL4,
    P_A_LEVEL, P_A_KF, P_A_PAN, P_A_PAN_KF, P_A_DMODE, P_A_DTIME, P_A_CURVE, P_A_VELO, P_A_ON, P_A_OFF,
    P_A_TKF, P_AT1, P_AL1, P_AT2, P_AL2, P_AT3, P_AL3, P_AT4,
    P_DRY, P_REV_SEND, P_CHO_SEND, P_OUT_SEL,
    PARAM_COUNT
};

#define CP(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, false, false}
#define TP(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, true, false}
#define TW(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, true, true}
/* A controller source's four destination / sense pairs */
#define CTRL(off)                                                                                  \
    TP("DEST 1", off, K_LIST, 0, 12, 0, dest_labels), TP("SENSE 1", off + 1, K_SIGNED, 1, 127, 64, 0), \
    TP("DEST 2", off + 2, K_LIST, 0, 12, 0, dest_labels), TP("SENSE 2", off + 3, K_SIGNED, 1, 127, 64, 0), \
    TP("DEST 3", off + 4, K_LIST, 0, 12, 0, dest_labels), TP("SENSE 3", off + 5, K_SIGNED, 1, 127, 64, 0), \
    TP("DEST 4", off + 6, K_LIST, 0, 12, 0, dest_labels), TP("SENSE 4", off + 7, K_SIGNED, 1, 127, 64, 0)

static const jv_param_t params[PARAM_COUNT] = {
    [P_VELO_SW] = CP("VELO SW", 0x0C, K_SWITCH, 0, 1, 0, 0),
    [P_REV_TYPE] = CP("REV TYPE", 0x0D, K_LIST, 0, 7, 0, reverb_labels),
    [P_REV_LEVEL] = CP("REV LEVEL", 0x0E, K_NUM, 0, 127, 0, 0),
    [P_REV_TIME] = CP("REV TIME", 0x0F, K_NUM, 0, 127, 0, 0),
    [P_DELAY_FB] = CP("DELAY FB", 0x10, K_NUM, 0, 127, 0, 0),
    [P_CHO_TYPE] = CP("CHO TYPE", 0x11, K_LIST, 0, 2, 0, chorus_labels),
    [P_CHO_LEVEL] = CP("CHO LEVEL", 0x12, K_NUM, 0, 127, 0, 0),
    [P_CHO_DEPTH] = CP("CHO DEPTH", 0x13, K_NUM, 0, 127, 0, 0),
    [P_CHO_RATE] = CP("CHO RATE", 0x14, K_NUM, 0, 127, 0, 0),
    [P_CHO_FB] = CP("CHO FDBK", 0x15, K_NUM, 0, 127, 0, 0),
    [P_CHO_OUT] = CP("CHO OUT", 0x16, K_LIST, 0, 1, 0, chorus_out_labels),
    [P_ANALOG] = CP("ANALOG FEEL", 0x17, K_NUM, 0, 127, 0, 0),
    [P_LEVEL] = CP("LEVEL", 0x18, K_NUM, 0, 127, 0, 0),
    [P_PAN] = CP("PAN", 0x19, K_PAN, 0, 127, 0, 0),
    [P_BEND_DN] = CP("BEND DOWN", 0x1A, K_SIGNED, 16, 64, 64, 0),
    [P_BEND_UP] = CP("BEND UP", 0x1B, K_NUM, 0, 12, 0, 0),
    [P_KEY_ASSIGN] = CP("KEY ASSIGN", 0x1C, K_LIST, 0, 1, 0, key_assign_labels),
    [P_SOLO_LEG] = CP("LEGATO", 0x1D, K_SWITCH, 0, 1, 0, 0),
    [P_PORTA_SW] = CP("PORTA", 0x1E, K_SWITCH, 0, 1, 0, 0),
    [P_PORTA_MODE] = CP("PORTA MODE", 0x1F, K_LIST, 0, 1, 0, porta_mode_labels),
    [P_PORTA_TYPE] = CP("PORTA TYPE", 0x20, K_LIST, 0, 1, 0, porta_type_labels),
    [P_PORTA_TIME] = CP("PORTA TIME", 0x21, K_NUM, 0, 127, 0, 0),

    [P_GROUP] = TP("WAVE GROUP", 0x00, K_LIST, 0, 1, 0, group_labels),  /* no PCM (data card) waves: not tested */
    [P_WAVE] = TW("WAVE NUMBER", 0x01, K_PLUS1, 0, 254, 0, 0),
    [P_TONE_SW] = TP("TONE", 0x03, K_SWITCH, 0, 1, 0, 0),
    [P_FXM_SW] = TP("FXM", 0x04, K_SWITCH, 0, 1, 0, 0),
    [P_FXM_DEPTH] = TP("FXM DEPTH", 0x05, K_PLUS1, 0, 15, 0, 0),
    [P_VEL_LO] = TP("VELO LOWER", 0x06, K_NUM, 0, 127, 0, 0),
    [P_VEL_HI] = TP("VELO UPPER", 0x07, K_NUM, 0, 127, 0, 0),
    [P_VOL_SW] = TP("VOLUME SW", 0x08, K_SWITCH, 0, 1, 0, 0),
    [P_HOLD_SW] = TP("HOLD-1 SW", 0x09, K_SWITCH, 0, 1, 0, 0),
    [P_MOD] = CTRL(0x0A),
    [P_AFT] = CTRL(0x12),
    [P_EXPR] = CTRL(0x1A),

    [P_L1_FORM] = TP("LFO FORM", 0x22, K_LIST, 0, 5, 0, lfo_form_labels),
    [P_L1_OFFSET] = TP("LFO OFFSET", 0x23, K_LIST, 0, 4, 0, lfo_offset_labels),
    [P_L1_SYNC] = TP("LFO SYNC", 0x24, K_SWITCH, 0, 1, 0, 0),
    [P_L1_RATE] = TP("LFO RATE", 0x25, K_NUM, 0, 127, 0, 0),
    [P_L1_DELAY] = TW("LFO DELAY", 0x26, K_DELAY, 0, 128, 0, 0),
    [P_L1_FADE_POL] = TP("FADE OUT", 0x28, K_LIST, 0, 1, 0, fade_labels),
    [P_L1_FADE_TIME] = TP("FADE TIME", 0x29, K_NUM, 0, 127, 0, 0),
    [P_L1_PITCH] = TP("PITCH DEPTH", 0x2A, K_SIGNED10, 4, 124, 64, 0),
    [P_L1_TVF] = TP("TVF DEPTH", 0x2B, K_SIGNED, 1, 127, 64, 0),
    [P_L1_TVA] = TP("TVA DEPTH", 0x2C, K_SIGNED, 1, 127, 64, 0),
    [P_L2_FORM] = TP("LFO FORM", 0x2D, K_LIST, 0, 5, 0, lfo_form_labels),
    [P_L2_OFFSET] = TP("LFO OFFSET", 0x2E, K_LIST, 0, 4, 0, lfo_offset_labels),
    [P_L2_SYNC] = TP("LFO SYNC", 0x2F, K_SWITCH, 0, 1, 0, 0),
    [P_L2_RATE] = TP("LFO RATE", 0x30, K_NUM, 0, 127, 0, 0),
    [P_L2_DELAY] = TW("LFO DELAY", 0x31, K_DELAY, 0, 128, 0, 0),
    [P_L2_FADE_POL] = TP("FADE OUT", 0x33, K_LIST, 0, 1, 0, fade_labels),
    [P_L2_FADE_TIME] = TP("FADE TIME", 0x34, K_NUM, 0, 127, 0, 0),
    [P_L2_PITCH] = TP("PITCH DEPTH", 0x35, K_SIGNED10, 4, 124, 64, 0),
    [P_L2_TVF] = TP("TVF DEPTH", 0x36, K_SIGNED, 1, 127, 64, 0),
    [P_L2_TVA] = TP("TVA DEPTH", 0x37, K_SIGNED, 1, 127, 64, 0),

    [P_COARSE] = TP("COARSE", 0x38, K_SIGNED, 16, 112, 64, 0),
    [P_FINE] = TP("FINE", 0x39, K_SIGNED, 14, 114, 64, 0),
    [P_RANDOM] = TP("RANDOM", 0x3A, K_LIST, 0, 15, 0, random_labels),
    [P_PKF] = TP("KEY FOLLOW", 0x3B, K_LIST, 0, 15, 0, keyf_labels),
    [P_PE_VELO] = TP("VELO LEVEL", 0x3C, K_SIGNED, 1, 127, 64, 0),
    [P_PE_ON] = TP("VELO ON T", 0x3D, K_LIST, 0, 14, 0, sense_labels),
    [P_PE_OFF] = TP("VELO OFF T", 0x3E, K_LIST, 0, 14, 0, sense_labels),
    [P_PE_TKF] = TP("TIME KF", 0x3F, K_LIST, 0, 14, 0, sense_labels),
    [P_PE_DEPTH] = TP("PENV DEPTH", 0x40, K_SIGNED, 52, 76, 64, 0),
    [P_PT1] = TP("T1", 0x41, K_EG, 0, 127, 0, 0),
    [P_PL1] = TP("L1", 0x42, K_EG, 1, 127, 64, 0),
    [P_PT2] = TP("T2", 0x43, K_EG, 0, 127, 0, 0),
    [P_PL2] = TP("L2", 0x44, K_EG, 1, 127, 64, 0),
    [P_PT3] = TP("T3", 0x45, K_EG, 0, 127, 0, 0),
    [P_PL3] = TP("L3", 0x46, K_EG, 1, 127, 64, 0),
    [P_PT4] = TP("T4", 0x47, K_EG, 0, 127, 0, 0),
    [P_PL4] = TP("L4", 0x48, K_EG, 1, 127, 64, 0),

    [P_F_MODE] = TP("FILTER", 0x49, K_LIST, 0, 2, 0, filter_labels),
    [P_CUTOFF] = TP("CUTOFF", 0x4A, K_NUM, 0, 127, 0, 0),
    [P_RESO] = TP("RESONANCE", 0x4B, K_NUM, 0, 127, 0, 0),
    [P_RESO_MODE] = TP("RESO MODE", 0x4C, K_LIST, 0, 1, 0, reso_labels),
    [P_FKF] = TP("CUTOFF KF", 0x4D, K_LIST, 0, 15, 0, keyf_labels),
    [P_F_CURVE] = TP("VELO CURVE", 0x4E, K_PLUS1, 0, 6, 0, 0),
    [P_F_VELO] = TP("VELO LEVEL", 0x4F, K_SIGNED, 1, 127, 64, 0),
    [P_F_ON] = TP("VELO ON T", 0x50, K_LIST, 0, 14, 0, sense_labels),
    [P_F_OFF] = TP("VELO OFF T", 0x51, K_LIST, 0, 14, 0, sense_labels),
    [P_F_TKF] = TP("TIME KF", 0x52, K_LIST, 0, 14, 0, sense_labels),
    [P_F_DEPTH] = TP("ENV DEPTH", 0x53, K_SIGNED, 1, 127, 64, 0),
    [P_FT1] = TP("T1", 0x54, K_EG, 0, 127, 0, 0),
    [P_FL1] = TP("L1", 0x55, K_EG, 0, 127, 0, 0),
    [P_FT2] = TP("T2", 0x56, K_EG, 0, 127, 0, 0),
    [P_FL2] = TP("L2", 0x57, K_EG, 0, 127, 0, 0),
    [P_FT3] = TP("T3", 0x58, K_EG, 0, 127, 0, 0),
    [P_FL3] = TP("L3", 0x59, K_EG, 0, 127, 0, 0),
    [P_FT4] = TP("T4", 0x5A, K_EG, 0, 127, 0, 0),
    [P_FL4] = TP("L4", 0x5B, K_EG, 0, 127, 0, 0),

    [P_A_LEVEL] = TP("LEVEL", 0x5C, K_NUM, 0, 127, 0, 0),
    [P_A_KF] = TP("LEVEL KF", 0x5D, K_LIST, 0, 14, 0, sense_labels),
    [P_A_PAN] = TW("PAN", 0x5E, K_PAN, 0, 128, 0, 0),
    [P_A_PAN_KF] = TP("PAN KF", 0x60, K_LIST, 0, 14, 0, sense_labels),
    [P_A_DMODE] = TP("DELAY MODE", 0x61, K_LIST, 0, 2, 0, dmode_labels),
    [P_A_DTIME] = TW("DELAY TIME", 0x62, K_DELAY, 0, 128, 0, 0),
    [P_A_CURVE] = TP("VELO CURVE", 0x64, K_PLUS1, 0, 6, 0, 0),
    [P_A_VELO] = TP("VELO LEVEL", 0x65, K_SIGNED, 1, 127, 64, 0),
    [P_A_ON] = TP("VELO ON T", 0x66, K_LIST, 0, 14, 0, sense_labels),
    [P_A_OFF] = TP("VELO OFF T", 0x67, K_LIST, 0, 14, 0, sense_labels),
    [P_A_TKF] = TP("TIME KF", 0x68, K_LIST, 0, 14, 0, sense_labels),
    [P_AT1] = TP("T1", 0x69, K_EG, 0, 127, 0, 0),
    [P_AL1] = TP("L1", 0x6A, K_EG, 0, 127, 0, 0),
    [P_AT2] = TP("T2", 0x6B, K_EG, 0, 127, 0, 0),
    [P_AL2] = TP("L2", 0x6C, K_EG, 0, 127, 0, 0),
    [P_AT3] = TP("T3", 0x6D, K_EG, 0, 127, 0, 0),
    [P_AL3] = TP("L3", 0x6E, K_EG, 0, 127, 0, 0),
    [P_AT4] = TP("T4", 0x6F, K_EG, 0, 127, 0, 0),

    [P_DRY] = TP("DRY LEVEL", 0x70, K_NUM, 0, 127, 0, 0),
    [P_REV_SEND] = TP("REVERB SEND", 0x71, K_NUM, 0, 127, 0, 0),
    [P_CHO_SEND] = TP("CHORUS SEND", 0x72, K_NUM, 0, 127, 0, 0),
    [P_OUT_SEL] = TP("OUTPUT", 0x73, K_LIST, 0, 1, 0, output_labels),
};

/* ---- pages ---- */

#define _ SYNTH_NONE
enum {
    PG_WAVE,
    PG_PATCH, PG_PORTA, PG_CHORUS,
    PG_PITCH, PG_PENV, PG_PENV_VELO,
    PG_TVF, PG_TVF_ENV, PG_TVF_VELO,
    PG_TVA, PG_TVA_ENV, PG_TVA_SCALE,
    PG_LFO1, PG_LFO2,
    PG_MOD, PG_AFTER, PG_EXPR,
};
#define NO_SW {_, _, _, _, _, _, _, _}
static const synth_page_t pages[] = {
    /* A WAVE: the most used, so first */
    [PG_WAVE] = {0, "WAVE", {P_GROUP, P_WAVE, P_FXM_DEPTH, P_VEL_LO, P_VEL_HI, _, _, _},
                 {P_TONE_SW, _, P_FXM_SW, _, _, P_VOL_SW, P_HOLD_SW, _}},
    /* B COMMON */
    [PG_PATCH] = {1, "PATCH", {P_LEVEL, P_PAN, P_ANALOG, P_BEND_DN, P_BEND_UP, _, _, _},
                  {_, _, _, _, _, P_VELO_SW, _, _}},
    /* Key assign and portamento, then the reverb: eight knobs */
    [PG_PORTA] = {1, "PORTA / REVERB",
                  {P_KEY_ASSIGN, P_PORTA_MODE, P_PORTA_TYPE, P_PORTA_TIME, P_REV_TYPE, P_REV_LEVEL, P_REV_TIME,
                   P_DELAY_FB},
                  {P_SOLO_LEG, P_PORTA_SW, _, _, _, _, _, _}},
    [PG_CHORUS] = {1, "CHORUS", {P_CHO_TYPE, P_CHO_LEVEL, P_CHO_DEPTH, P_CHO_RATE, P_CHO_FB, P_CHO_OUT, _, _},
                   NO_SW},
    /* C PITCH, D TVF, E TVA: the same three-page shape, so the page stays */
    [PG_PITCH] = {2, "PITCH", {P_COARSE, P_FINE, P_PKF, P_RANDOM, P_PE_DEPTH, _, _, _}, NO_SW, 1},
    [PG_PENV] = {2, "PITCH ENV", {P_PT1, P_PL1, P_PT2, P_PL2, P_PT3, P_PL3, P_PT4, P_PL4}, NO_SW, 1},
    [PG_PENV_VELO] = {2, "PITCH ENV VELO", {P_PE_VELO, P_PE_ON, P_PE_OFF, P_PE_TKF, _, _, _, _}, NO_SW, 1},
    [PG_TVF] = {3, "TVF", {P_F_MODE, P_CUTOFF, P_RESO, P_RESO_MODE, P_F_DEPTH, P_FKF, _, _}, NO_SW, 1},
    [PG_TVF_ENV] = {3, "TVF ENV", {P_FT1, P_FL1, P_FT2, P_FL2, P_FT3, P_FL3, P_FT4, P_FL4}, NO_SW, 1},
    [PG_TVF_VELO] = {3, "TVF ENV VELO", {P_F_CURVE, P_F_VELO, P_F_ON, P_F_OFF, P_F_TKF, _, _, _}, NO_SW, 1},
    /* TVA and the output sends it feeds: the level chain on one page */
    [PG_TVA] = {4, "TVA / OUTPUT", {P_A_LEVEL, P_A_PAN, P_A_DMODE, P_A_DTIME, P_DRY, P_REV_SEND, P_CHO_SEND, P_OUT_SEL},
                NO_SW, 1},
    [PG_TVA_ENV] = {4, "TVA ENV", {P_AT1, P_AL1, P_AT2, P_AL2, P_AT3, P_AL3, P_AT4, _}, NO_SW, 1},
    [PG_TVA_SCALE] = {4, "TVA SCALING", {P_A_KF, P_A_PAN_KF, P_A_CURVE, P_A_VELO, P_A_ON, P_A_OFF, P_A_TKF, _},
                      NO_SW, 1},
    /* F LFO 1, G LFO 2: everything of one on a page (fade direction is a
     * switch, ON = OUT) */
    [PG_LFO1] = {5, "LFO 1", {P_L1_FORM, P_L1_RATE, P_L1_PITCH, P_L1_TVF, P_L1_TVA, P_L1_DELAY, P_L1_OFFSET,
                              P_L1_FADE_TIME},
                 {_, P_L1_SYNC, _, _, _, _, _, P_L1_FADE_POL}},
    [PG_LFO2] = {6, "LFO 2", {P_L2_FORM, P_L2_RATE, P_L2_PITCH, P_L2_TVF, P_L2_TVA, P_L2_DELAY, P_L2_OFFSET,
                              P_L2_FADE_TIME},
                 {_, P_L2_SYNC, _, _, _, _, _, P_L2_FADE_POL}},
    /* H CONTROLLERS */
    [PG_MOD] = {7, "MODULATION", {P_MOD, P_MOD + 1, P_MOD + 2, P_MOD + 3, P_MOD + 4, P_MOD + 5, P_MOD + 6, P_MOD + 7},
                NO_SW},
    [PG_AFTER] = {7, "AFTERTOUCH", {P_AFT, P_AFT + 1, P_AFT + 2, P_AFT + 3, P_AFT + 4, P_AFT + 5, P_AFT + 6, P_AFT + 7},
                  NO_SW},
    [PG_EXPR] = {7, "EXPRESSION", {P_EXPR, P_EXPR + 1, P_EXPR + 2, P_EXPR + 3, P_EXPR + 4, P_EXPR + 5, P_EXPR + 6, P_EXPR + 7},
                 NO_SW},
};
#undef _

/* ---- settings ---- */

/* Setup shows four settings per synth: the unit number, the CARD and what
 * IMPORT does; the volume is set with the Volume button */
#define SETTING_UNIT 1
#define SETTING_CARD 2
#define SETTING_IMPORT 3
#define SETTING_VOLUME_JV 4
static const synth_setting_t settings[] = {
    /* The unit's System "Patch Receive Channel" (its default: 1) */
    [SETTING_CHANNEL] = {"PATCH CH", SET_CHANNEL, 0, 15, 0},
    /* Exclusive unit number (the unit's MIDI function), device ID = unit - 1 */
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
    /* The expansion board in the unit (SR-JV80-nn, 1-19 as they are; 20-22 =
     * 97-99, the Experience boards; 0 = none): the unit can't tell, so it's set by hand. Each board has its own patch library, as
     * its patches need its waveforms. */
    [SETTING_CARD] = {"CARD", SET_NUMBER, 0, 22, 0},
    /* What IMPORT brings in, into the library of the CARD set:
     * 0 = the unit's internal patches as a board's (001-064, replacing the
     *     earlier import, protected),
     * 1 = the internal patches as own ones (from 200, kept; copies of ones
     *     already in the library are skipped),
     * 2 = the presets A and B (065-192 in the main library, shown with
     *     every board) */
    [SETTING_IMPORT] = {"IMPORT", SET_NUMBER, 0, 2, 0},
    [SETTING_VOLUME_JV] = {"VOLUME", SET_NUMBER, 0, 127, 100},  /* not on Setup: Volume button */
};

/* The board's own number: 1-19, or 97-99 */
static int card_number(void)
{
    return jv880_card_number(synth_setting(SETTING_CARD));
}

static void setting_note(int setting, char *buf, size_t len)
{
    if (setting == SETTING_IMPORT) {
        static const char *const what[] = {"INT: BOARD", "INT: OWN", "PRESETS A+B"};
        snprintf(buf, len, "%s", what[synth_setting(SETTING_IMPORT) % 3]);
    } else if (setting == SETTING_CARD) {
        snprintf(buf, len, "%s", jv880_card_names[synth_setting(SETTING_CARD) % 23]);
    }
}

/* ---- state ---- */

static uint8_t patch[PATCH_LEN], init_patch[PATCH_LEN];
static uint8_t as_read[PATCH_LEN];  /* the patch as the unit last gave it, for Rec + Erase */
static bool as_read_ok;
static bool known[PATCH_LEN];
static bool init_read;
static uint8_t tone_sel = 1;  /* bit n = tone n+1 edited */

static int first_tone(void)
{
    for (int i = 0; i < 4; i++) {
        if (tone_sel & (1 << i)) {
            return i;
        }
    }
    return 0;
}

/* Index in the patch buffer of p for tone t (or the common part) */
static int byte_of(int p, int t)
{
    return params[p].tone ? COMMON_LEN + t * TONE_LEN + params[p].off : params[p].off;
}

static int tone_count(int p) { return params[p].tone ? 4 : 1; }

static void read_init(void)
{
    if (init_read) {
        return;
    }
    FILE *f = fopen(STORAGE_ROOT "/jv880/000 Init", "rb");
    if (f) {
        init_read = fread(init_patch, 1, PATCH_LEN, f) == PATCH_LEN;
        fclose(f);
    }
}

static int get(const uint8_t *buf, int p, int t)
{
    int at = byte_of(p, t);
    return params[p].wide ? buf[at] << 4 | buf[at + 1] : buf[at];
}

static int raw_value(int p, int t) { return get(patch, p, t); }

static bool value_known(int p) { return known[byte_of(p, first_tone())]; }

/* ---- sending ---- */

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }

/* Address of patch buffer index `at` when the patch starts at `base` */
static uint32_t address_of(uint32_t base, int at)
{
    if (at < COMMON_LEN) {
        return base + at;
    }
    int t = (at - COMMON_LEN) / TONE_LEN, w = (at - COMMON_LEN) % TONE_LEN;
    return JV880_TONE_ADDR(base, t) + w;
}

/* The other way round: buffer index of an address in the patch at base, or -1 */
static int index_in(uint32_t addr, uint32_t base)
{
    int o = (int)(addr - base);
    if (o < 0) {
        return -1;
    }
    if (o < COMMON_LEN) {
        return o;
    }
    o -= JV880_ADDR(0, 0, 8, 0);  /* tone 1 starts at +08 00 */
    int t = o / TONE_GAP, w = o % TONE_GAP;
    return o >= 0 && t < 4 && w < TONE_LEN ? COMMON_LEN + t * TONE_LEN + w : -1;
}

static int index_of(uint32_t addr) { return index_in(addr, JV880_TEMP_PATCH); }

/* DT1 of patch bytes [from, from + len) - inside one block - to a patch at base */
static void send_bytes_to(uint32_t base, int from, int len)
{
    uint8_t msg[13 + TONE_LEN];
    int n = jv880_dt1(device_id(), address_of(base, from), patch + from, len, msg);
    midi_send_sysex(msg, n);
}

static void send_bytes(int from, int len) { send_bytes_to(JV880_TEMP_PATCH, from, len); }

/* Whole patches go out a block at a time from tick(), so the MIDI queue
 * never overflows and the unit gets time for each */
static uint8_t pending;  /* bit n = block n still to send */
static uint32_t pending_base = JV880_TEMP_PATCH;
static int64_t next_send_us;
#define SEND_GAP_US 12000
#define BYTE_US 330  /* 31250 baud DIN */

static void block_range(int b, int *from, int *len)
{
    *from = b ? COMMON_LEN + (b - 1) * TONE_LEN : 0;
    *len = b ? TONE_LEN : COMMON_LEN;
}

static void send_patch(uint32_t base)
{
    pending = (1 << BLOCKS) - 1;
    pending_base = base;
    next_send_us = 0;
}

/* ---- the library: one directory per expansion board ---- */

#define CARD_COUNT 100
#define COMMON_DIR STORAGE_ROOT "/jv880"  /* no board; also what every board's library shows */
static char card_dir[40];

/* /lfs/jv880 for no board, /lfs/jv880/cardNN for SR-JV80-NN (patches
 * saved with it set stay with it); the main directory's patches - Init,
 * the presets - show in every one */
static const char *library_dir(void)
{
    int card = card_number();
    if (card <= 0 || card >= CARD_COUNT) {
        return COMMON_DIR;
    }
    snprintf(card_dir, sizeof(card_dir), COMMON_DIR "/card%02d", card);
    mkdir(card_dir, 0777);
    return card_dir;
}

static const char *common_dir(void) { return COMMON_DIR; }

/* ---- import: the unit's internal patches into the library ----
 * An expansion board's 64 patches come into the unit's internal memory, so
 * reading I01-I64 (plain RQ1s, manual p. 223) saves them as 001-064 of the
 * board's library. Needs the JV-880's MIDI OUT connected. */
#define IMPORT_SLOTS 64      /* internal: saved as 001-064 */
#define PRESET_SLOTS 128     /* A01-A64, B01-B64: saved as 065-192 */
#define PRESET_FIRST 65
#define PRESET_SETTLE_US 150000  /* after selecting a preset, before asking */
#define FIRST_USER_PATCH 200  /* below: imported patches, protected */
#define IMPORT_TIMEOUT_US 1500000
typedef enum { IMP_IDLE, IMP_SELECT, IMP_SETTLE, IMP_ASK, IMP_REPLY } imp_stage_t;
static imp_stage_t imp_stage;
static int imp_slot, imp_block, imp_saved, imp_skipped;
static bool imp_own, imp_presets;
static int imp_slots;
static uint8_t imp_original[PATCH_LEN];  /* presets: the editor's patch, put back after */
static bool imp_original_ok;
static int imp_next;  /* own patches: the next number to save as */
#define IMPORT_HASHES 512
static uint32_t imp_hash[IMPORT_HASHES];  /* own patches: what the library holds */
static int imp_hash_n;
static uint8_t imp_buf[PATCH_LEN];
static int64_t imp_deadline;

static uint32_t internal_base(int slot) { return JV880_ADDR(1, 0x40 + slot, 0x20, 0); }
/* Where the current import's patch is read from: a preset is selected into
 * the temporary patch first */
static uint32_t import_base(void) { return imp_presets ? JV880_TEMP_PATCH : internal_base(imp_slot); }

/* ---- reading back (optional: needs the JV-880's MIDI OUT on the
 * Maschine's MIDI IN) ---- */

static bool read_asked;  /* Auto pressed: the reply replaces the patch */
static int read_block = BLOCKS;  /* next block to ask for, BLOCKS = idle */
/* A read asks for the blocks one by one; ones that didn't arrive (the first
 * request after boot often gets lost) are asked for again, twice at most */
static uint8_t read_got;       /* bit n = block n arrived whole */
static int read_tries;
static int64_t read_check_at;  /* when to see what's missing, 0 = not waiting */
#define READ_CHECK_US 300000
#define READ_TRIES 3
#define ALL_BLOCKS ((1 << BLOCKS) - 1)

static void request_block(int b)
{
    int from, len;
    block_range(b, &from, &len);
    uint8_t msg[17];
    int n = jv880_rq1(device_id(), address_of(JV880_TEMP_PATCH, from), len, msg);
    ESP_LOGI(TAG, "RQ1 block %d (%d bytes, device %02X)", b, len, device_id());
    midi_send_sysex(msg, n);
}


static uint32_t patch_hash(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < PATCH_LEN; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void read_patch(void);
/* Rec + Erase on the unit's own sound: back to what it was when read */
static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(patch, as_read, PATCH_LEN);
    memset(known, 1, PATCH_LEN);
    send_patch(JV880_TEMP_PATCH);
    return true;
}

static int unknown_count(void);

static void import_finish(const char *why)
{
    imp_stage = IMP_IDLE;
    if (imp_presets) {
        /* The temporary patch is whatever preset came last: the editor's own back */
        if (imp_original_ok) {
            memcpy(patch, imp_original, PATCH_LEN);
            send_patch(JV880_TEMP_PATCH);
        } else {
            read_patch();
        }
    }
    synth_patches_changed();
    char buf[48];
    if (why) {
        snprintf(buf, sizeof(buf), "%s", why);
    } else {
        snprintf(buf, sizeof(buf), "IMPORTED %d (%d COPIES SKIPPED)", imp_saved, imp_skipped);
    }
    synth_notice(buf);
    ESP_LOGI(TAG, "import: %s (%d saved)", why ? why : "done", imp_saved);
}

static void import_save(void)
{
    char name[13];
    memcpy(name, imp_buf, 12);
    name[12] = 0;
    for (int i = 11; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    for (char *c = name; *c; c++) {
        if (*c == '/' || *c < 32 || *c > 126) {
            *c = '_';
        }
    }
    int number = imp_presets ? PRESET_FIRST + imp_slot : imp_slot + 1;
    if (imp_own && !imp_presets) {
        /* Own patches are kept: next free number, and not twice */
        uint32_t h = patch_hash(imp_buf);
        for (int i = 0; i < imp_hash_n; i++) {
            if (imp_hash[i] == h) {
                imp_skipped++;
                return;
            }
        }
        if (imp_hash_n < IMPORT_HASHES) {
            imp_hash[imp_hash_n++] = h;
        }
        number = imp_next++;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%03d %s", imp_presets ? common_dir() : library_dir(), number,
             name[0] ? name : "Patch");
    if (imp_presets) {
        ESP_LOGI(TAG, "preset %c%02d: %s", imp_slot < 64 ? 'A' : 'B', imp_slot % 64 + 1, name);
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        imp_saved += fwrite(imp_buf, 1, PATCH_LEN, f) == PATCH_LEN;
        fclose(f);
    }
}

static void import_start(void)
{
    if (imp_stage != IMP_IDLE) {
        return;
    }
    imp_presets = synth_setting(SETTING_IMPORT) == 2;
    imp_own = synth_setting(SETTING_IMPORT) == 1;
    imp_slots = imp_presets ? PRESET_SLOTS : IMPORT_SLOTS;
    imp_next = FIRST_USER_PATCH;
    imp_hash_n = 0;
    const char *target = imp_presets ? common_dir() : library_dir();
    int first = imp_presets ? PRESET_FIRST : 1, last = imp_presets ? PRESET_FIRST + PRESET_SLOTS - 1 : IMPORT_SLOTS;
    DIR *dir = opendir(target);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", target, e->d_name);
        if (!imp_own && n >= first && n <= last) {
            remove(path);  /* the earlier import of this kind */
        } else if (imp_own && n >= FIRST_USER_PATCH) {
            if (n >= imp_next) {
                imp_next = n + 1;
            }
            FILE *f = fopen(path, "rb");
            uint8_t buf[PATCH_LEN];
            if (f && imp_hash_n < IMPORT_HASHES && fread(buf, 1, PATCH_LEN, f) == PATCH_LEN) {
                imp_hash[imp_hash_n++] = patch_hash(buf);
            }
            if (f) {
                fclose(f);
            }
        }
    }
    if (dir) {
        closedir(dir);
    }
    imp_original_ok = imp_presets && unknown_count() == 0;
    if (imp_original_ok) {
        memcpy(imp_original, patch, PATCH_LEN);
    }
    pending = 0;
    read_block = BLOCKS;
    imp_slot = imp_block = imp_saved = imp_skipped = 0;
    imp_stage = imp_presets ? IMP_SELECT : IMP_ASK;
    synth_notice("IMPORT: asking the JV-880...");
}

static void import_request(int64_t now)
{
    int from, len;
    block_range(imp_block, &from, &len);
    uint8_t msg[17];
    int n = jv880_rq1(device_id(), address_of(import_base(), from), len, msg);
    midi_send_sysex(msg, n);
    imp_stage = IMP_REPLY;
    imp_deadline = now + IMPORT_TIMEOUT_US;
}

/* The block imp_block of the slot arrived (reply bytes stored in imp_buf) */
static void import_block_done(void)
{
    if (++imp_block < BLOCKS) {
        imp_stage = IMP_ASK;
        return;
    }
    import_save();
    char buf[32];
    snprintf(buf, sizeof(buf), "IMPORT %d / %d", imp_slot + 1, imp_slots);
    synth_notice(buf);
    imp_block = 0;
    if (++imp_slot >= imp_slots) {
        import_finish(NULL);
    } else {
        imp_stage = imp_presets ? IMP_SELECT : IMP_ASK;
    }
}

static void tick(int64_t now)
{
    if (imp_stage == IMP_SELECT) {
        /* Bank Select MSB 81 = presets, then the Program Change: 0-63 A, 64-127 B
         * (manual p. 219-220); the unit's Rx switches for both must be on */
        int ch = synth_setting(SETTING_CHANNEL);
        midi_send(0xB0 | ch, 0, 81);
        midi_send(0xC0 | ch, imp_slot, 0);
        imp_stage = IMP_SETTLE;
        imp_deadline = now + PRESET_SETTLE_US;
        return;
    }
    if (imp_stage == IMP_SETTLE) {
        if (now > imp_deadline) {
            imp_stage = IMP_ASK;
        }
        return;
    }
    if (imp_stage == IMP_ASK) {
        import_request(now);
        return;
    }
    if (imp_stage == IMP_REPLY) {
        if (now > imp_deadline) {
            import_finish("IMPORT stopped: no reply - JV-880 MIDI OUT connected?");
        }
        return;
    }
    if (pending && now >= next_send_us) {
        int b = __builtin_ctz(pending);
        int from, len;
        block_range(b, &from, &len);
        pending &= ~(1 << b);
        send_bytes_to(pending_base, from, len);
        next_send_us = now + SEND_GAP_US + (int64_t)len * BYTE_US;
    } else if (!pending && read_block < BLOCKS && now >= next_send_us) {
        while (read_block < BLOCKS && (read_got >> read_block & 1)) {
            read_block++;  /* this one is in already */
        }
        if (read_block < BLOCKS) {
            request_block(read_block++);
            next_send_us = now + 60000;
        }
        if (read_block >= BLOCKS) {
            read_check_at = now + READ_CHECK_US;
        }
    } else if (read_check_at && read_block >= BLOCKS && now >= read_check_at) {
        read_check_at = 0;
        if ((read_got & ALL_BLOCKS) != ALL_BLOCKS && read_tries < READ_TRIES) {
            read_tries++;
            read_block = 0;  /* the missing ones again */
        }
    }
}

static void read_patch(void)
{
    if (imp_stage != IMP_IDLE) {
        import_finish("IMPORT stopped");  /* Auto during an import stops it */
        return;
    }
    read_asked = true;
    read_block = 0;
    read_got = 0;
    read_tries = 1;
    read_check_at = 0;
    pending = 0;
    next_send_us = 0;
}

static void sysex_in(const uint8_t *d, size_t len)
{
    uint32_t addr;
    const uint8_t *data;
    int n = jv880_parse_dt1(d, len, device_id(), &addr, &data);
    if (n < 1) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes, device %02X model %02X command %02X", (int)len,
                 len > 2 ? d[2] : 0, len > 3 ? d[3] : 0, len > 4 ? d[4] : 0);
        return;
    }
    if (imp_stage == IMP_REPLY) {
        int from, len;
        block_range(imp_block, &from, &len);
        for (int i = 0; i < n; i++) {
            int at = index_in(addr + i, import_base());
            if (at >= from && at < from + len) {
                imp_buf[at] = data[i];
            }
        }
        if (index_in(addr, import_base()) == from && n == len) {
            import_block_done();
        }
        return;
    }
    bool any = false;
    for (int i = 0; i < n; i++) {
        int at = index_of(addr + i);
        if (at >= 0) {
            patch[at] = data[i];
            known[at] = true;
            any = true;
        }
    }
    if (!any) {
        return;
    }
    ESP_LOGI(TAG, "read %d bytes at %06X", n, (unsigned)addr);
    for (int b = 0; b < BLOCKS; b++) {
        int from, blen;
        block_range(b, &from, &blen);
        if (index_of(addr) == from && n == blen) {
            read_got |= 1 << b;
        }
    }
    if (read_got & 1) {
        synth_state_received();  /* the patch's name is in: no fallback */
    }
    if ((read_got & ALL_BLOCKS) == ALL_BLOCKS) {
        memcpy(as_read, patch, PATCH_LEN);
        as_read_ok = true;
    }
    if (read_asked && index_of(addr) == 0) {
        /* Auto (read) on purpose: the unit's patch replaces whatever library
         * patch was loaded */
        read_asked = false;
        synth_sound_replaced();
    }
    if (index_of(addr) == 0) {
        char name[13], buf[32];
        memcpy(name, patch, 12);
        name[12] = 0;
        snprintf(buf, sizeof(buf), "READ: %s", name);
        synth_notice(buf);
    }
    synth_redraw();
}

/* ---- writing into the unit's own memory (Store: TO JV-880) ----
 * The patch goes to an internal patch (I01-I64, manual p. 223) with plain
 * DT1s, name first */

static void unit_slot_name(int slot, char *buf, size_t len) { snprintf(buf, len, "I%02d", slot + 1); }

static void unit_store(int slot, const char *name)
{
    for (int i = 0; i < 12; i++) {
        char c = *name ? *name++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
        known[i] = true;
    }
    read_init();
    for (int i = 0; i < PATCH_LEN; i++) {
        if (!known[i]) {
            patch[i] = init_patch[i];  /* never set: the init patch's */
        }
    }
    send_bytes(0, 12);
    send_patch(JV880_ADDR(1, 0x40 + slot, 0x20, 0));
    char slot_name[8], buf[40];
    unit_slot_name(slot, slot_name, sizeof(slot_name));
    snprintf(buf, sizeof(buf), "WRITING TO %s", slot_name);
    synth_notice(buf);
}

/* ---- editing ---- */

/* Stores v for tone t (or the common part) and sends it */
static void set_one(int p, int t, int v)
{
    int at = byte_of(p, t);
    int len = 1;
    if (params[p].wide) {
        patch[at] = v >> 4 & 0x0F;
        patch[at + 1] = v & 0x0F;
        known[at + 1] = true;
        len = 2;
    } else {
        patch[at] = v;
    }
    known[at] = true;
    send_bytes(at, len);
}

static void set_value(int p, int v)
{
    for (int t = 0; t < tone_count(p); t++) {
        if (!params[p].tone || (tone_sel & (1 << t))) {
            set_one(p, t, v);
        }
    }
}

static int value_or_init(int p, int t)
{
    if (known[byte_of(p, t)]) {
        return raw_value(p, t);
    }
    read_init();
    return get(init_patch, p, t);
}

/* ---- formatting ---- */

static const char *param_name(int p) { return params[p].name; }

static void param_format(int p, char *buf, size_t len)
{
    const jv_param_t *pp = &params[p];
    if (!value_known(p)) {
        snprintf(buf, len, "--");
        return;
    }
    int v = raw_value(p, first_tone());
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_SIGNED10:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", (v - pp->center) * 10);
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
    case K_DELAY:
        if (v >= 128) {
            snprintf(buf, len, "KEY-OFF");
        } else {
            snprintf(buf, len, "%d", v);
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
    const jv_param_t *pp = &params[p];
    if (!value_known(p) || pp->kind == K_EG || pp->kind == K_LIST || pp->kind == K_SWITCH || p == P_WAVE) {
        return -1;
    }
    return (raw_value(p, first_tone()) - pp->min) * 1000 / (pp->max - pp->min);
}

static bool param_on(int p) { return value_known(p) && raw_value(p, first_tone()); }

static int param_units(int p)
{
    int range = params[p].max - params[p].min;
    return range <= 1 ? 80 : range <= 4 ? 70 : range <= 16 ? 40 : range <= 30 ? 25
           : p == P_WAVE ? WAVE_UNITS : 10;  /* picking a wave among up to 255: slow */
}

static int wave_count(int group);
static int wave_nearest(int group, int number);
static bool wave_exists(int group, int number);

/* With several tones selected, each moves by the same steps from its own
 * value, so their differences stay */
static bool param_step(int p, int steps)
{
    bool changed = false;
    for (int t = 0; t < tone_count(p); t++) {
        if (params[p].tone && !(tone_sel & (1 << t))) {
            continue;
        }
        int v = value_or_init(p, t), nv = v + steps;
        int group = p == P_WAVE ? value_or_init(P_GROUP, t) : 0;
        int max = p == P_WAVE ? wave_count(group) - 1 : params[p].max;
        nv = nv < params[p].min ? params[p].min : nv > max ? max : nv;
        if (p == P_WAVE && !wave_exists(group, nv)) {
            /* a gap in the list: on to the next wave there is, in the way
             * the knob turns, or stay */
            int dir = steps > 0 ? 1 : -1, k = nv;
            while (k >= 0 && k <= max && !wave_exists(group, k)) {
                k += dir;
            }
            nv = k >= 0 && k <= max ? k : v;
        }
        if (nv != v || !known[byte_of(p, t)] || p == P_GROUP) {
            set_one(p, t, nv);
            changed = true;
            if (p == P_GROUP) {
                /* another group: the wave goes to the nearest one it has,
                 * and is sent too, so the sound changes at once */
                int w = value_or_init(P_WAVE, t);
                set_one(P_WAVE, t, wave_nearest(nv, w));
                synth_redraw();  /* the wave's column too */
            }
        }
    }
    return changed;
}

static void param_toggle(int p) { set_value(p, value_known(p) ? !raw_value(p, first_tone()) : 0); }

static bool param_reset(int p)
{
    read_init();
    int v = get(init_patch, p, first_tone());
    if (value_known(p) && raw_value(p, first_tone()) == v) {
        return false;
    }
    set_value(p, v);
    return true;
}

/* ---- pictures ---- */

#define BAND_Y 14
#define BAND_H 38

/* Envelope columns: "T1 50" at the bottom, the envelope gets the rest */
static bool column_draw(int p, int disp, int x)
{
    if (params[p].kind != K_EG) {
        return false;
    }
    char val[8];
    param_format(p, val, sizeof(val));
    jv880_draw_eg_text(disp, x, params[p].name, val);
    return true;
}

static int pv(int p) { return raw_value(p, first_tone()); }

static void page_draw(int pg, int disp)
{
    if (pg != PG_PENV && pg != PG_TVF_ENV && pg != PG_TVA_ENV) {
        return;
    }
    int first = pg == PG_PENV ? P_PT1 : pg == PG_TVF_ENV ? P_FT1 : P_AT1;
    if (!known[byte_of(first, first_tone())]) {
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

static int wave_count(int group) { return jv880_wave_count(group, card_number()); }
static bool wave_exists(int group, int number) { return jv880_wave_exists(group, number, card_number()); }
static int wave_nearest(int group, int number) { return jv880_wave_nearest(group, number, card_number()); }

/* The wave number, and its name below when there is one; never a bar */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    if (p != P_WAVE || !value_known(p)) {
        return SYNTH_DRAW_NONE;
    }
    int t = first_tone();
    char num[8];
    param_format(p, num, sizeof(num));
    jv880_draw_wave(disp, x, y, w, num, raw_value(P_GROUP, t), raw_value(P_WAVE, t), card_number());
    return SYNTH_DRAW_FULL;
}

/* ---- tones ---- */

static bool shift_group(int g)
{
    if (g > 3) {
        return false;
    }
    uint8_t sel = tone_sel ^ (1 << g);
    if (!sel) {
        return false;  /* at least one tone stays selected */
    }
    tone_sel = sel;
    return true;
}

static int shift_group_led(int g) { return g > 3 ? -1 : (tone_sel >> g) & 1; }

static void tone_tag(char *tag)
{
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (tone_sel & (1 << i)) {
            tag[n++] = '1' + i;
        }
    }
    tag[n] = 0;
}

/* Step arrows: every selected tone moves one along (1 -> 2, 1+3 -> 2+4),
 * round from 4 to 1 */
static void tone_step(int d)
{
    tone_sel = d > 0 ? ((tone_sel << 1) | (tone_sel >> 3)) & 0x0F : ((tone_sel >> 1) | (tone_sel << 3)) & 0x0F;
    char buf[16], tag[8];
    tone_tag(tag);
    snprintf(buf, sizeof(buf), "TONE %s", tag);
    synth_notice(buf);
}

static void header_tag(char *buf, size_t len)
{
    char tag[8];
    tone_tag(tag);
    snprintf(buf, len, "T%s", tag);
}

/* The patch's name, when known (e.g. read back) */
static bool sound_name(char *buf, size_t len)
{
    if (!known[0]) {
        return false;
    }
    char name[13];
    memcpy(name, patch, 12);
    name[12] = 0;
    for (int i = 11; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(buf, len, "%s", name);
    return true;
}

/* The patch must be the one the unit plays: Patch mode (System Common, panel
 * mode, manual p. 223) */
static bool activate(void)
{
    uint8_t mode = 1, msg[16];
    int n = jv880_dt1(device_id(), JV880_SYSTEM_COMMON, &mode, 1, msg);
    midi_send_sysex(msg, n);
    return true;
}

/* ---- patches ---- */

static bool patch_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memset(known, 1, PATCH_LEN);
    read_block = BLOCKS;  /* a read still going would bring the unit's old patch back */
    read_check_at = 0;
    send_patch(JV880_TEMP_PATCH);
    return true;
}

static bool patch_save(const char *path)
{
    /* The patch's own name (12 characters) follows the file's */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    read_init();
    for (int i = 0; i < PATCH_LEN; i++) {
        if (!known[i]) {
            patch[i] = init_patch[i];  /* never set: saved as the init patch's */
        }
    }
    for (int i = 0; i < 12; i++) {
        char c = *title ? *title++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
    }
    memset(known, 1, PATCH_LEN);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    return ok;
}

/* A patch using the expansion board's waves (wave group EXP) saved with no
 * CARD set would show up whatever board is in */
static const char *save_warning(void)
{
    if (synth_setting(SETTING_CARD) != 0) {
        return NULL;
    }
    for (int t = 0; t < 4; t++) {
        if (patch[COMMON_LEN + t * TONE_LEN + params[P_GROUP].off] == 1) {
            return "EXP WAVES: SET CARD (SETUP)";
        }
    }
    return NULL;
}

static int unknown_count(void)
{
    int n = 0;
    for (int i = 0; i < PATCH_LEN; i++) {
        n += !known[i];
    }
    return n;
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"JV-880 PATCH", {"Patch mode: one patch = common part +", "four tones. Group A-H are blocks:",
                      "A WAVE  B COMMON  C PITCH  D TVF", "E TVA+OUTPUT  F LFO1  G LFO2  H CTRLS",
                      "Seldom used parameters: last pages.", "Set the unit's patch receive channel",
                      "as PATCH CH (Setup)."},
     {BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2, BTN_GROUP_A + 3, BTN_GROUP_A + 4, BTN_GROUP_A + 5,
      BTN_GROUP_A + 6, BTN_GROUP_A + 7}},
    {"TONES", {"STEP arrows: the next tone.", "SHIFT + A-D: pick several - a knob then",
               "moves them all alike (differences stay).", "While SHIFT is held, A-D show the pick;",
               "the header shows it too (T13).", "TONE on A WAVE mutes a tone."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_SHIFT, BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2,
      BTN_GROUP_A + 3, -1}},
    {"READ, WRITE", {"AUTO: read the patch from the JV-880", "(its MIDI OUT to the Maschine).",
                     "REC > TO JV-880 (twice): write the", "patch into an internal patch, I01-I64.",
                     "Selecting this synth sets Patch mode."},
     {BTN_AUTO, BTN_REC, -1, -1, -1, -1, -1, -1}},
    {"EXPANSION BOARDS", {"A board's 64 patches load into the unit's", "Internal memory. Set CARD (Setup) to the",
                          "board in the unit: each has its own", "library, others are hidden. CONTROL >",
                          "IMPORT (twice) saves I01-I64 as 001-064.", "IMPORT 1: own patches instead, kept from", "200. IMPORT 2: presets A+B, 065-192."},
     {BTN_CONTROL, -1, -1, -1, -1, -1, -1, -1}},
    {"VOLUME", {"Hold VOLUME and turn the main encoder:", "Main Volume (CC7) on the patch channel."},
     {BTN_VOLUME, -1, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_jv880 = {
    .name = "JV-880",
    .device = "JV-880",
    .entry = "PATCH",
    .key = "jv880",
    .patch_dir = STORAGE_ROOT "/jv880",
    .default_patch = "000 Init",
    .patch_dir_fn = library_dir,
    .common_dir_fn = common_dir,
    .first_user_patch = FIRST_USER_PATCH,
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
    .save_warning = save_warning,
    .unknown_count = unknown_count,
    .keep_state = true,  /* selecting it reads the unit's patch, sends nothing */
    .activate = activate,
    .activate_ms = 150,
    .volume_cc = 7,
    .volume_setting = SETTING_VOLUME_JV,
    .page_draw = page_draw,
    .column_draw = column_draw,
    .instance_step = tone_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_patch,
    .sysex_in = sysex_in,
    .tick = tick,
    .action_name = "IMPORT",
    .action = import_start,
    .unit_store_label = "TO JV-880",
    .unit_store_slots = 64,
    .unit_slot_name = unit_slot_name,
    .unit_store = unit_store,
    .theme_bright = {127, 90, 0},  /* amber */
    .theme_dim = {14, 9, 0},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
