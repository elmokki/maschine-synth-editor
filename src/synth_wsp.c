/*
 * Korg Wavestation SR, PATCH: a patch is four waves (A-D; 1, 2 or 4 of them
 * in use: "structure") mixed by the vector mix envelope, each wave with its own
 * tuning, pitch, two LFOs, amp envelope, envelope 1, filter and pan.
 *
 * Layout: the Group buttons are functions, the wave (A-D) is picked with Shift +
 * Group A-D or the transport Step arrows (which skip waves the structure doesn't
 * use): A WAVE (2 pages: wave and tuning; pan and the bus switches), B PITCH,
 * C AMP (envelope; modulation), D ENV 1, E FILTER, F LFO (LFO 1; LFO 2; their
 * modulation), G MIX (vector: point, X, Y, time, loop and the two live joystick
 * controllers; modulation), H PATCH (structure, hard sync).
 *
 * Every value goes out as a SysEx parameter change (value as ASCII text, one
 * a message, 40 ms apart) to the patch the SR's part 1 plays (BANK and PATCH
 * settings; selecting the synth, or changing them, runs Edisyn's selection
 * sequence). Wave parameters need the wave chosen first (CURRENT_WAVE), the
 * mix envelope's the point (MIX_ENV_POINT). Selecting a library patch sends
 * all of it, which takes a while. Not stored in the unit (the SR has no
 * single-patch write; storing goes through bank dumps - not done yet).
 *
 * The vector is also played live with Control Change 16 (x, A-C) and 17 (y,
 * B-D), the SR's defaults for its MIDI joystick (MIDI REMAP page).
 *
 * A patch file is the patch as the SR keeps it: 426 bytes, big-endian
 * (layout: Edisyn's KorgWavestationPatch.java). The messages and parameter
 * numbers are from the SR's SysEx appendix (Reference Guide, appendix 4)
 * as Edisyn uses them.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "ws_banks.h"

static const char *TAG = "wsp";

#define PATCH_LEN 426
#define HEADER_LEN 90
#define WAVE_LEN 84
#define NAME_LEN 16
#define NAME_CHARS 15
#define WAVES 4
#define SEND_GAP_US 40000

/* ---- parameter numbers (appendix 4) ---- */
#define PRM_CURRENT_BANK 0
#define PRM_CURRENT_PROG 2
#define PRM_MIDI_MODE 4
#define PRM_CURRENT_PART 56
#define PRM_PART_PATCH_BANK 57
#define PRM_PART_PATCH_NUM 58
#define PRM_PART_PATCH_NAME 59
#define PRM_CURRENT_WAVE 79
#define PRM_MIX_ENV_POINT 193
#define PRM_MIX_ENV_RATE 194
#define PRM_MIX_ENV_X 195
#define PRM_MIX_ENV_Y 196
#define PRM_PERF_MIDI_MODE 427
#define WAVE_PARAM_BASE 147  /* wave A bank; B 154, C 161, D 168 (7 apart): bank, num, name, level, coarse, fine, slope */
#define BUS_PARAM_BASE 362   /* wave A bus A; 4 a wave */

/* Bank numbers: the SR's own (parameter messages, dump headers) and the wave bank (Edisyn's order: RAM1-3,
 * ROM4-11, Card) as the patch data keeps it (low 2 bits + Bank_Exp bits) */
#define to_ws_bank ws_to_ws_bank
#define from_ws_exp_bank ws_from_ws_exp_bank
#define to_ws_exp_bank ws_to_ws_exp_bank
#define bank_names ws_bank_names

static const char *const source_names[13] = {"KBD LIN", "KBD CTR", "VEL LIN", "VEL EXP", "LFO 1", "LFO 2",
                                             "ENV 1", "AFTERTCH", "AT+MODW", "MOD WHL", "MIDI 1", "MIDI 2",
                                             "MOD PED"};
static const char *const shape_names[5] = {"TRIANGLE", "SQUARE", "SAWTOOTH", "RAMP", "RANDOM"};
static const char *const loop_names[7] = {"OFF", "0>3", "1>3", "2>3", "0<>3", "1<>3", "2<>3"};
static const char *const structure_names[3] = {"1: A", "2: A C", "4: A-D"};

/* ---- fields ---- */

typedef enum {
    K_NUM, K_SIGNED, K_SOURCE, K_SHAPE, K_SWITCH, K_BANK, K_WAVE, K_XY, K_BEND, K_RAMPT, K_REPEATS, K_LOOP,
    K_STRUCT, K_SLOPE,
} kind_t;

#define F_WAVE 1    /* per wave: at WAVE_LEN * wave + HEADER_LEN + off */
#define F_NEEDW 2   /* the message needs CURRENT_WAVE first */
#define F_SGN 4     /* a signed byte */
#define F_W16 8     /* a big-endian word (the wave number) */

typedef struct {
    const char *name;  /* fits a 64 px display column */
    kind_t kind;
    uint16_t param;    /* waves: wave A's; the others are 7 (bus 4) further each, as for NEEDW ones the same */
    int16_t min, max;
    uint8_t off, flags, shift, mask;
    int8_t point;      /* mix envelope point the message needs first, -1 = none */
} field_t;

#define PL(name, kind, param, min, max, off, point) {name, kind, param, min, max, off, 0, 0, 0xFF, point}
#define PLS(name, kind, param, min, max, off) {name, kind, param, min, max, off, F_SGN, 0, 0xFF, -1}
#define W(name, kind, param, min, max, off, flags, shift, mask) \
    {name, kind, param, min, max, off, F_WAVE | (flags), shift, mask, -1}

enum {
    /* patch */
    F_STRUCT, F_SYNC, F_LOOP, F_REPEATS,
    F_MIXT1, F_MIXT2, F_MIXT3, F_MIXT4,
    F_MIXX0, F_MIXX1, F_MIXX2, F_MIXX3, F_MIXX4, F_MIXY0, F_MIXY1, F_MIXY2, F_MIXY3, F_MIXY4,
    F_XS1, F_XA1, F_XS2, F_XA2, F_YS1, F_YA1, F_YS2, F_YA2,
    PATCH_FIELDS,
    /* wave */
    F_BANK = PATCH_FIELDS, F_WAVE_NUM, F_LEVEL, F_COARSE, F_FINE, F_SLOPE, F_BUSA, F_BUSB, F_BUSC, F_BUSD,
    F_PAN_KBD, F_PAN_VEL,
    F_BEND, F_RAMP_AMT, F_RAMP_TIME, F_RAMP_VEL, F_PS1, F_PA1, F_PS2, F_PA2,
    F_L1_SHAPE, F_L1_SYNC, F_L1_RATE, F_L1_AMT, F_L1_DELAY, F_L1_FADE, F_L1_RSRC, F_L1_RAMT, F_L1_DSRC, F_L1_DAMT,
    F_L2_SHAPE, F_L2_SYNC, F_L2_RATE, F_L2_AMT, F_L2_DELAY, F_L2_FADE, F_L2_RSRC, F_L2_RAMT, F_L2_DSRC, F_L2_DAMT,
    F_AS1, F_AA1, F_AS2, F_AA2,
    F_AE_L0, F_AE_T1, F_AE_L1, F_AE_T2, F_AE_L2, F_AE_T3, F_AE_L3, F_AE_T4, F_AE_VELA, F_AE_VELR, F_AE_KBD,
    F_E1_L0, F_E1_T1, F_E1_L1, F_E1_T2, F_E1_L2, F_E1_T3, F_E1_L3, F_E1_T4, F_E1_L4, F_E1_VELA, F_E1_VELR,
    F_E1_KBD,
    F_FS1, F_FA1, F_FS2, F_FA2, F_CUTOFF, F_EXCITER, F_FKBD,
    FIELD_COUNT,
    /* not in the patch: the vector's point and the point's values, the live joystick */
    P_POINT = FIELD_COUNT, P_MIX_X, P_MIX_Y, P_MIX_TIME, P_JOY_X, P_JOY_Y, P_RST_STEP, P_RST_VEC,
    PARAM_COUNT
};

static const field_t fields[FIELD_COUNT] = {
    [F_STRUCT] = PL("STRUCTURE", K_STRUCT, 77, 0, 2, 86, -1),
    [F_SYNC] = PL("HARD SYNC", K_SWITCH, 78, 0, 1, 87, -1),
    [F_LOOP] = PL("LOOP", K_LOOP, 201, 0, 6, 77, -1),
    [F_REPEATS] = PL("REPEATS", K_REPEATS, 202, 0, 127, 76, -1),
    [F_MIXT1] = PL("TIME 1", K_NUM, PRM_MIX_ENV_RATE, 0, 99, 16, 1),
    [F_MIXT2] = PL("TIME 2", K_NUM, PRM_MIX_ENV_RATE, 0, 99, 17, 2),
    [F_MIXT3] = PL("TIME 3", K_NUM, PRM_MIX_ENV_RATE, 0, 99, 18, 3),
    [F_MIXT4] = PL("TIME 4", K_NUM, PRM_MIX_ENV_RATE, 0, 99, 19, 4),
    [F_MIXX0] = PL("AC 0", K_XY, PRM_MIX_ENV_X, 0, 254, 66, 0),
    [F_MIXX1] = PL("AC 1", K_XY, PRM_MIX_ENV_X, 0, 254, 67, 1),
    [F_MIXX2] = PL("AC 2", K_XY, PRM_MIX_ENV_X, 0, 254, 68, 2),
    [F_MIXX3] = PL("AC 3", K_XY, PRM_MIX_ENV_X, 0, 254, 69, 3),
    [F_MIXX4] = PL("AC 4", K_XY, PRM_MIX_ENV_X, 0, 254, 70, 4),
    [F_MIXY0] = PL("BD 0", K_XY, PRM_MIX_ENV_Y, 0, 254, 71, 0),
    [F_MIXY1] = PL("BD 1", K_XY, PRM_MIX_ENV_Y, 0, 254, 72, 1),
    [F_MIXY2] = PL("BD 2", K_XY, PRM_MIX_ENV_Y, 0, 254, 73, 2),
    [F_MIXY3] = PL("BD 3", K_XY, PRM_MIX_ENV_Y, 0, 254, 74, 3),
    [F_MIXY4] = PL("BD 4", K_XY, PRM_MIX_ENV_Y, 0, 254, 75, 4),
    [F_XS1] = PL("AC SRC 1", K_SOURCE, 203, 0, 12, 78, -1),
    [F_XA1] = PLS("AC AMT 1", K_SIGNED, 204, -127, 127, 79),
    [F_XS2] = PL("AC SRC 2", K_SOURCE, 205, 0, 12, 80, -1),
    [F_XA2] = PLS("AC AMT 2", K_SIGNED, 206, -127, 127, 81),
    [F_YS1] = PL("BD SRC 1", K_SOURCE, 207, 0, 12, 82, -1),
    [F_YA1] = PLS("BD AMT 1", K_SIGNED, 208, -127, 127, 83),
    [F_YS2] = PL("BD SRC 2", K_SOURCE, 209, 0, 12, 84, -1),
    [F_YA2] = PLS("BD AMT 2", K_SIGNED, 210, -127, 127, 85),

    /* wave (param numbers: wave A's, +7 a wave for the first six, +4 a wave for the buses) */
    [F_BANK] = W("BANK", K_BANK, 147, 0, 11, 2, 0, 0, 0xFF),
    [F_WAVE_NUM] = W("WAVE", K_WAVE, 148, 0, 517, 3, F_W16, 0, 0xFF),
    [F_LEVEL] = W("LEVEL", K_NUM, 150, 0, 99, 73, F_SGN, 0, 0xFF),
    [F_COARSE] = W("COARSE", K_SIGNED, 151, -24, 24, 0, F_SGN, 0, 0xFF),
    [F_FINE] = W("FINE", K_SIGNED, 152, -99, 99, 1, F_SGN, 0, 0xFF),
    [F_SLOPE] = W("SLOPE", K_SLOPE, 153, -100, 100, 5, F_SGN, 0, 0xFF),
    [F_BUSA] = W("BUS A", K_SWITCH, 362, 0, 1, 82, 0, 0, 1),
    [F_BUSB] = W("BUS B", K_SWITCH, 363, 0, 1, 82, 0, 1, 1),
    [F_BUSC] = W("BUS C", K_SWITCH, 364, 0, 1, 82, 0, 2, 1),
    [F_BUSD] = W("BUS D", K_SWITCH, 365, 0, 1, 82, 0, 3, 1),
    [F_PAN_KBD] = W("PAN KBD", K_SIGNED, 146, -127, 127, 64, F_NEEDW | F_SGN, 0, 0xFF),
    [F_PAN_VEL] = W("PAN VEL", K_SIGNED, 145, -127, 127, 65, F_NEEDW | F_SGN, 0, 0xFF),
    [F_BEND] = W("BEND", K_BEND, 85, -1, 12, 47, F_NEEDW | F_SGN, 0, 0xFF),
    [F_RAMP_AMT] = W("RAMP AMT", K_SIGNED, 86, -127, 127, 70, F_NEEDW | F_SGN, 0, 0xFF),
    [F_RAMP_TIME] = W("RAMP TIME", K_RAMPT, 87, 0, 100, 71, F_NEEDW, 0, 0xFF),
    [F_RAMP_VEL] = W("RAMP VEL", K_SIGNED, 88, -127, 127, 72, F_NEEDW | F_SGN, 0, 0xFF),
    [F_PS1] = W("SOURCE 1", K_SOURCE, 89, 0, 12, 48, F_NEEDW, 0, 0xFF),
    [F_PA1] = W("AMOUNT 1", K_NUM, 90, 0, 99, 49, F_NEEDW, 0, 0xFF),
    [F_PS2] = W("SOURCE 2", K_SOURCE, 91, 0, 12, 50, F_NEEDW, 0, 0xFF),
    [F_PA2] = W("AMOUNT 2", K_NUM, 92, 0, 99, 51, F_NEEDW, 0, 0xFF),
    [F_L1_SHAPE] = W("SHAPE", K_SHAPE, 127, 0, 4, 10, F_NEEDW, 0, 0x7F),
    [F_L1_SYNC] = W("SYNC", K_SWITCH, 128, 0, 1, 10, F_NEEDW, 7, 1),
    [F_L1_RATE] = W("RATE", K_NUM, 125, 0, 99, 6, F_NEEDW, 0, 0xFF),
    [F_L1_AMT] = W("AMOUNT", K_NUM, 126, 0, 127, 7, F_NEEDW, 0, 0xFF),
    [F_L1_DELAY] = W("DELAY", K_NUM, 129, 0, 99, 8, F_NEEDW, 0, 0xFF),
    [F_L1_FADE] = W("FADE IN", K_NUM, 130, 0, 99, 9, F_NEEDW, 0, 0xFF),
    [F_L1_RSRC] = W("RATE SRC", K_SOURCE, 133, 0, 12, 11, F_NEEDW, 0, 0xFF),
    [F_L1_RAMT] = W("RATE AMT", K_SIGNED, 134, -127, 127, 12, F_NEEDW | F_SGN, 0, 0xFF),
    [F_L1_DSRC] = W("DEPTH SRC", K_SOURCE, 131, 0, 12, 13, F_NEEDW, 0, 0xFF),
    [F_L1_DAMT] = W("DEPTH AMT", K_SIGNED, 132, -127, 127, 14, F_NEEDW | F_SGN, 0, 0xFF),
    [F_L2_SHAPE] = W("SHAPE", K_SHAPE, 137, 0, 4, 19, F_NEEDW, 0, 0x7F),
    [F_L2_SYNC] = W("SYNC", K_SWITCH, 138, 0, 1, 19, F_NEEDW, 7, 1),
    [F_L2_RATE] = W("RATE", K_NUM, 135, 0, 99, 15, F_NEEDW, 0, 0xFF),
    [F_L2_AMT] = W("AMOUNT", K_NUM, 136, 0, 127, 16, F_NEEDW, 0, 0xFF),
    [F_L2_DELAY] = W("DELAY", K_NUM, 139, 0, 99, 17, F_NEEDW, 0, 0xFF),
    [F_L2_FADE] = W("FADE IN", K_NUM, 140, 0, 99, 18, F_NEEDW, 0, 0xFF),
    [F_L2_RSRC] = W("RATE SRC", K_SOURCE, 143, 0, 12, 20, F_NEEDW, 0, 0xFF),
    [F_L2_RAMT] = W("RATE AMT", K_SIGNED, 144, -127, 127, 21, F_NEEDW | F_SGN, 0, 0xFF),
    [F_L2_DSRC] = W("DEPTH SRC", K_SOURCE, 141, 0, 12, 22, F_NEEDW, 0, 0xFF),
    [F_L2_DAMT] = W("DEPTH AMT", K_SIGNED, 142, -127, 127, 23, F_NEEDW | F_SGN, 0, 0xFF),
    [F_AS1] = W("SOURCE 1", K_SOURCE, 119, 0, 12, 60, F_NEEDW, 0, 0xFF),
    [F_AA1] = W("AMOUNT 1", K_SIGNED, 120, -127, 127, 61, F_NEEDW | F_SGN, 0, 0xFF),
    [F_AS2] = W("SOURCE 2", K_SOURCE, 121, 0, 12, 62, F_NEEDW, 0, 0xFF),
    [F_AA2] = W("AMOUNT 2", K_SIGNED, 122, -127, 127, 63, F_NEEDW | F_SGN, 0, 0xFF),
    [F_AE_L0] = W("LEVEL 0", K_NUM, 110, 0, 99, 38, F_NEEDW, 0, 0xFF),
    [F_AE_T1] = W("TIME 1", K_NUM, 114, 0, 99, 34, F_NEEDW, 0, 0xFF),
    [F_AE_L1] = W("LEVEL 1", K_NUM, 111, 0, 99, 39, F_NEEDW, 0, 0xFF),
    [F_AE_T2] = W("TIME 2", K_NUM, 115, 0, 99, 35, F_NEEDW, 0, 0xFF),
    [F_AE_L2] = W("LEVEL 2", K_NUM, 112, 0, 99, 40, F_NEEDW, 0, 0xFF),
    [F_AE_T3] = W("TIME 3", K_NUM, 116, 0, 99, 36, F_NEEDW, 0, 0xFF),
    [F_AE_L3] = W("LEVEL 3", K_NUM, 113, 0, 99, 41, F_NEEDW, 0, 0xFF),
    [F_AE_T4] = W("TIME 4", K_NUM, 117, 0, 99, 37, F_NEEDW, 0, 0xFF),
    [F_AE_VELA] = W("VEL AMT", K_SIGNED, 118, -127, 127, 57, F_NEEDW | F_SGN, 0, 0xFF),
    [F_AE_VELR] = W("VEL ATTACK", K_SIGNED, 123, -127, 127, 58, F_NEEDW | F_SGN, 0, 0xFF),
    [F_AE_KBD] = W("KBD DECAY", K_SIGNED, 124, -127, 127, 59, F_NEEDW | F_SGN, 0, 0xFF),
    [F_E1_L0] = W("LEVEL 0", K_NUM, 100, 0, 99, 28, F_NEEDW, 0, 0xFF),
    [F_E1_T1] = W("TIME 1", K_NUM, 105, 0, 99, 24, F_NEEDW, 0, 0xFF),
    [F_E1_L1] = W("LEVEL 1", K_NUM, 101, 0, 99, 29, F_NEEDW, 0, 0xFF),
    [F_E1_T2] = W("TIME 2", K_NUM, 106, 0, 99, 25, F_NEEDW, 0, 0xFF),
    [F_E1_L2] = W("LEVEL 2", K_NUM, 102, 0, 99, 30, F_NEEDW, 0, 0xFF),
    [F_E1_T3] = W("TIME 3", K_NUM, 107, 0, 99, 26, F_NEEDW, 0, 0xFF),
    [F_E1_L3] = W("LEVEL 3", K_NUM, 103, 0, 99, 31, F_NEEDW, 0, 0xFF),
    [F_E1_T4] = W("TIME 4", K_NUM, 108, 0, 99, 27, F_NEEDW, 0, 0xFF),
    [F_E1_L4] = W("LEVEL 4", K_NUM, 104, 0, 99, 32, F_NEEDW, 0, 0xFF),
    [F_E1_VELA] = W("VEL AMT", K_SIGNED, 109, -127, 127, 33, F_NEEDW | F_SGN, 0, 0xFF),
    [F_E1_VELR] = W("VEL ATTACK", K_SIGNED, 347, -127, 127, 68, F_NEEDW | F_SGN, 0, 0xFF),
    [F_E1_KBD] = W("KBD RATE", K_SIGNED, 348, -127, 127, 69, F_NEEDW | F_SGN, 0, 0xFF),
    [F_FS1] = W("SOURCE 1", K_SOURCE, 96, 0, 12, 53, F_NEEDW, 0, 0xFF),
    [F_FA1] = W("AMOUNT 1", K_SIGNED, 97, -127, 127, 54, F_NEEDW | F_SGN, 0, 0xFF),
    [F_FS2] = W("SOURCE 2", K_SOURCE, 98, 0, 12, 55, F_NEEDW, 0, 0xFF),
    [F_FA2] = W("AMOUNT 2", K_SIGNED, 99, -127, 127, 56, F_NEEDW | F_SGN, 0, 0xFF),
    [F_CUTOFF] = W("CUTOFF", K_NUM, 93, 0, 99, 66, F_NEEDW, 0, 0xFF),
    [F_EXCITER] = W("EXCITER", K_NUM, 95, 0, 99, 67, F_NEEDW, 0, 0xFF),
    [F_FKBD] = W("KBD TRACK", K_SIGNED, 94, -127, 127, 52, F_NEEDW | F_SGN, 0, 0xFF),
};

/* ---- pages ---- */

#define _ SYNTH_NONE
enum {
    PG_WAVE, PG_PAN_BUS, PG_PITCH, PG_AMP_ENV, PG_AMP_MOD, PG_ENV1, PG_ENV1_2, PG_FILTER, PG_LFO1, PG_LFO2,
    PG_LFO_MOD, PG_VECTOR, PG_MIX_MOD, PG_PATCH
};
#define NO_SW {_, _, _, _, _, _, _, _}
static const synth_page_t pages[] = {
    [PG_WAVE] = {0, "WAVE", {F_BANK, F_WAVE_NUM, F_LEVEL, F_COARSE, F_FINE, F_SLOPE, _, _}, NO_SW},
    [PG_PAN_BUS] = {0, "PAN / BUS", {_, _, _, _, F_PAN_KBD, F_PAN_VEL, _, _},
                    {F_BUSA, F_BUSB, F_BUSC, F_BUSD, _, _, _, _}},
    [PG_PITCH] = {1, "PITCH", {F_BEND, F_RAMP_AMT, F_RAMP_TIME, F_RAMP_VEL, F_PS1, F_PA1, F_PS2, F_PA2}, NO_SW},
    [PG_AMP_ENV] = {2, "AMP ENVELOPE", {F_AE_L0, F_AE_T1, F_AE_L1, F_AE_T2, F_AE_L2, F_AE_T3, F_AE_L3, F_AE_T4},
                    NO_SW},
    [PG_AMP_MOD] = {2, "AMP MOD", {F_AE_VELA, F_AE_VELR, F_AE_KBD, _, F_AS1, F_AA1, F_AS2, F_AA2}, NO_SW},
    [PG_ENV1] = {3, "ENVELOPE 1", {F_E1_L0, F_E1_T1, F_E1_L1, F_E1_T2, F_E1_L2, F_E1_T3, F_E1_L3, F_E1_T4},
                 NO_SW},
    [PG_ENV1_2] = {3, "ENVELOPE 1 MOD", {F_E1_L4, F_E1_VELA, F_E1_VELR, F_E1_KBD, _, _, _, _}, NO_SW},
    [PG_FILTER] = {4, "FILTER", {F_CUTOFF, F_EXCITER, F_FKBD, _, F_FS1, F_FA1, F_FS2, F_FA2}, NO_SW},
    [PG_LFO1] = {5, "LFO 1", {F_L1_SHAPE, F_L1_RATE, F_L1_AMT, F_L1_DELAY, F_L1_FADE, _, _, _},
                 {_, F_L1_SYNC, _, _, _, _, _, _}},
    [PG_LFO2] = {5, "LFO 2", {F_L2_SHAPE, F_L2_RATE, F_L2_AMT, F_L2_DELAY, F_L2_FADE, _, _, _},
                 {_, F_L2_SYNC, _, _, _, _, _, _}},
    [PG_LFO_MOD] = {5, "LFO MODS", {F_L1_RSRC, F_L1_RAMT, F_L1_DSRC, F_L1_DAMT, F_L2_RSRC, F_L2_RAMT, F_L2_DSRC,
                                    F_L2_DAMT}, NO_SW},
    [PG_VECTOR] = {6, "VECTOR MIX", {P_POINT, P_MIX_X, P_MIX_Y, P_MIX_TIME, F_LOOP, F_REPEATS, P_JOY_X, P_JOY_Y},
                   {_, P_RST_STEP, P_RST_VEC, _, _, _, _, _}},
    [PG_MIX_MOD] = {6, "MIX MOD", {F_XS1, F_XA1, F_XS2, F_XA2, F_YS1, F_YA1, F_YS2, F_YA2}, NO_SW},
    [PG_PATCH] = {7, "PATCH", {F_STRUCT, _, _, _, _, _, _, _}, {_, F_SYNC, _, _, _, _, _, _}},
};
#undef _

/* ---- settings ---- */

#define SETTING_BANK 1
#define SETTING_PATCH 2
#define SETTING_BULK 3
#define SETTING_IMPORT 4
static const synth_setting_t settings[] = {
    [SETTING_CHANNEL] = {"CHANNEL", SET_CHANNEL, 0, 15, 0},
    /* The patch the SR's part 1 plays while editing: bank 0-11 (RAM 1-3, ROM 4-11, Card), number 0-34 */
    [SETTING_BANK] = {"EDIT BANK", SET_NUMBER, 0, 11, 0},
    [SETTING_PATCH] = {"EDIT PATCH", SET_NUMBER, 0, 34, 0},
    /* How a library patch goes to the SR: 1 = as one patch dump (fast), 0 = value by value (slow) */
    [SETTING_BULK] = {"LOAD AS", SET_NUMBER, 0, 1, 1},
    /* What IMPORT reads from the SR into the library: its banks, 35 patches each (about 10 s a bank) */
    [SETTING_IMPORT] = {"IMPORT", SET_NUMBER, 0, 3, 0},
};

static void setting_note(int setting, char *buf, size_t len)
{
    if (setting == SETTING_BANK) {
        int b = synth_setting(SETTING_BANK);
        snprintf(buf, len, "%s", bank_names[b < 0 ? 0 : b > 11 ? 11 : b]);
    } else if (setting == SETTING_BULK) {
        snprintf(buf, len, "%s", synth_setting(SETTING_BULK) ? "PATCH DUMP" : "PARAMETERS");
    } else if (setting == SETTING_IMPORT) {
        static const char *const what[] = {"RAM 1-3", "ROM 4-11", "CARD", "EDIT BANK"};
        snprintf(buf, len, "%s", what[synth_setting(SETTING_IMPORT) & 3]);
    }
}

/* ---- state ---- */

static uint8_t patch[PATCH_LEN], init_patch[PATCH_LEN];
static bool loaded;
static bool dirty[WAVES][FIELD_COUNT];  /* patch fields use wave 0 */
static bool name_dirty;
static int cur_wave;                    /* 0-3: the wave edited */
static int point;                       /* 0-4: the mix envelope point edited */
static int joy[2] = {64, 64};           /* the live vector (CC 16, 17) */
static int armed = -1;                  /* a reset button pressed once (P_RST_*), waiting for the second press */
static int64_t armed_until;
#define ARM_US 3000000
static int sent_wave = -1, sent_point = -1;  /* what the SR has selected, as far as we know */
static int sent_bank = -1, sent_num = -1;
static bool dump_pending;               /* the whole patch still to go to the SR as a dump */
static int64_t next_send_us;
/* the envelope trail's state (see below) */
static int trail_key = -1;       /* 0-7: (envelope: amp 0, envelope 1 1) * 4 + wave */
static int trail_lv[5], trail_tm[5];
static int64_t trail_taken, trail_until;
static bool from_unit;
static uint8_t as_read[PATCH_LEN];
static bool as_read_ok;

static void ensure(void)
{
    if (loaded) {
        return;
    }
    loaded = true;
    FILE *f = fopen(STORAGE_ROOT "/wsp/000 Init", "rb");
    if (f) {
        if (fread(init_patch, 1, PATCH_LEN, f) != PATCH_LEN) {
            memset(init_patch, 0, PATCH_LEN);
        }
        fclose(f);
    }
    memcpy(patch, init_patch, PATCH_LEN);
}

static int active_waves(void)
{
    ensure();
    int s = patch[86];
    return s == 0 ? 1 : s == 1 ? 2 : 4;
}

static bool wave_active(int w)
{
    int s = patch[86];
    return s >= 2 || w == 0 || (s == 1 && w == 2);
}

/* ---- reading and writing the patch bytes ---- */

static int base_of(const field_t *f, int w) { return (f->flags & F_WAVE) ? HEADER_LEN + WAVE_LEN * w + f->off : f->off; }

static int bank_get(int w)
{
    int ws = HEADER_LEN + WAVE_LEN * w, exp = patch[88];
    int bita = exp >> w & 1, bitb = exp >> (w + 4) & 1;
    int b = patch[ws + 2] & 3;
    return from_ws_exp_bank[bitb ? b + 8 : bita ? b + 4 : b];
}

static void bank_set(int w, int e)
{
    int x = to_ws_exp_bank[e];
    patch[HEADER_LEN + WAVE_LEN * w + 2] = x % 4;
    /* Bank_Exp: bit w = group 2, bit w + 4 = group 3 */
    int bits = (x >= 8 ? 16 : x >= 4 ? 1 : 0) << w;
    patch[88] = (patch[88] & ~(0x11 << w)) | bits;
}

static int get_field(int idx, int w)
{
    ensure();
    const field_t *f = &fields[idx];
    if (idx == F_BANK) {
        return bank_get(w);
    }
    int at = base_of(f, w);
    if (f->flags & F_W16) {
        return (patch[at] << 8 | patch[at + 1]) + patch[HEADER_LEN + WAVE_LEN * w + 83];
    }
    if (f->mask != 0xFF) {
        return patch[at] >> f->shift & f->mask;
    }
    return f->flags & F_SGN ? (int8_t)patch[at] : patch[at];
}

static void set_field(int idx, int w, int v)
{
    ensure();
    const field_t *f = &fields[idx];
    if (idx == F_BANK) {
        bank_set(w, v);
        return;
    }
    int at = base_of(f, w);
    if (f->flags & F_W16) {
        int num = v < 397 ? v : 396, exp = v < 397 ? 0 : v - 396;
        patch[at] = num >> 8;
        patch[at + 1] = num & 0xFF;
        patch[HEADER_LEN + WAVE_LEN * w + 83] = exp;
    } else if (f->mask != 0xFF) {
        patch[at] = (patch[at] & ~(f->mask << f->shift)) | (v & f->mask) << f->shift;
    } else {
        patch[at] = v;
    }
}

/* ---- tables the patch data derives from its values (Developer FAQ, as Edisyn) ---- */

static const uint16_t rate_tab[100] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 83, 85, 90, 95, 100,
    110, 120, 130, 140, 150, 180, 210, 240, 270, 300, 400, 500, 600, 700};

static void put16(int at, uint32_t v)
{
    patch[at] = v >> 8;
    patch[at + 1] = v;
}

static void put32(int at, uint32_t v)
{
    patch[at] = v >> 24;
    patch[at + 1] = v >> 16;
    patch[at + 2] = v >> 8;
    patch[at + 3] = v;
}

/* The mix envelope's counts and slopes and the LFOs' fade-in increments follow from the values; a patch dump
 * has them, so a saved file gets them right */
static void recompute_derived(void)
{
    static const uint8_t count_at[4][2] = {{20, 30}, {22, 28}, {24, 26}, {32, 32}};
    for (int i = 1; i <= 4; i++) {
        int rate = rate_tab[patch[15 + i] % 100];
        put16(count_at[i - 1][0], rate);
        put16(count_at[i - 1][1], rate);
        for (int axis = 0; axis < 2; axis++) {
            int a = patch[66 + 5 * axis + i - 1], b = patch[66 + 5 * axis + i];
            int64_t slope = ((int64_t)0x1000000 * (b - a)) / rate;
            put32(34 + 16 * axis + 4 * (i - 1), (uint32_t)(int32_t)slope);
        }
    }
    for (int w = 0; w < WAVES; w++) {
        int base = HEADER_LEN + WAVE_LEN * w;
        for (int l = 0; l < 2; l++) {
            int amt = patch[base + 7 + 9 * l], fade = patch[base + 9 + 9 * l] % 100;
            int64_t inc = fade == 0 ? (int64_t)0x7FFFFF * amt / 127 : (int64_t)0x7FFFFF * amt / ((int64_t)rate_tab[fade] * 127);
            put32(base + 74 + 4 * l, (uint32_t)inc);
        }
    }
}

/* ---- sending ---- */

static int channel(void) { return synth_setting(SETTING_CHANNEL) & 0x0F; }

/* One parameter change: F0 42 3n 28 <41|42|43> <param lo> <param hi> <value as ASCII text> 00 F7 */
static bool send_text(int param, const char *text)
{
    uint8_t msg[40];
    int n = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x42;
    msg[n++] = 0x30 + channel();
    msg[n++] = 0x28;
    msg[n++] = param >= 407 ? 0x43 : param >= 380 ? 0x42 : 0x41;
    msg[n++] = param & 127;
    msg[n++] = param >> 7 & 127;
    for (const char *c = text; *c && n < (int)sizeof(msg) - 2; c++) {
        msg[n++] = *c == ' ' ? 0x7F : *c;
    }
    msg[n++] = 0x00;
    msg[n++] = 0xF7;
    return midi_send_sysex(msg, n);
}

static bool send_param(int param, int value)
{
    char text[12];
    snprintf(text, sizeof(text), value > 0 ? "+%d" : "%d", value);
    return send_text(param, text);
}

/* ---- dumps: the patch (861 bytes) and a test performance (371) as the SR's own messages ---- */

static uint8_t dump_buf[900];

/* F0 42 3n 28 <type> <bank> <num> <data as nibbles, low then high> <sum of the nibbles> F7 */
static size_t build_dump(uint8_t type, int bank, int num, const uint8_t *data, size_t len)
{
    size_t n = 0;
    int sum = 0;
    dump_buf[n++] = 0xF0;
    dump_buf[n++] = 0x42;
    dump_buf[n++] = 0x30 + channel();
    dump_buf[n++] = 0x28;
    dump_buf[n++] = type;
    dump_buf[n++] = to_ws_bank[bank];
    dump_buf[n++] = num;
    for (size_t i = 0; i < len; i++) {
        dump_buf[n++] = data[i] & 0x0F;
        dump_buf[n++] = data[i] >> 4;
        sum += (data[i] & 0x0F) + (data[i] >> 4);
    }
    dump_buf[n++] = sum & 0x7F;
    dump_buf[n++] = 0xF7;
    return n;
}

static int edit_bank(void)
{
    int b = synth_setting(SETTING_BANK);
    return b < 0 ? 0 : b > 11 ? 11 : b;
}

static size_t build_patch_dump(void)
{
    ensure();
    recompute_derived();
    return build_dump(0x40, edit_bank(), synth_setting(SETTING_PATCH), patch, PATCH_LEN);
}

/* Edisyn's "test performance" (its Wavestation menu): a performance with a single part at full volume playing
 * the patch, the other parts off, no effects, the rest at the defaults; sent as RAM 1 #0. A single performance
 * dump goes to the SR's edit buffer, not into memory - as long as it isn't written. */
static size_t build_test_performance(void)
{
    uint8_t perf[181] = {0};
    memcpy(perf, "Edit            ", 16);
    perf[16] = 2;           /* FX 1: no effect, parallel */
    perf[17] = 2 | 128;     /* FX 2: no effect; extended effect mode (the one Edisyn writes) */
    perf[18] = 6 | 6 << 4;  /* mix 3 and 4: 5/5 */
    for (int pt = 0; pt < 8; pt++) {
        uint8_t *q = perf + 37 + 18 * pt;
        int exp = pt == 0 ? to_ws_exp_bank[edit_bank()] : 0;
        q[0] = exp % 4;                                        /* the patch's bank (low part) */
        q[1] = pt == 0 ? synth_setting(SETTING_PATCH) : 0xFF;  /* its number; -1 = off */
        q[2] = 99;                                             /* volume */
        q[3] = 50;                                             /* FX bus: A/B 50/50 */
        q[4] = (exp / 4 & 3) << 6 | 3 << 4 | 1 << 2 | 2;       /* bank (high part), play: both, polyphonic, last note */
        q[5] = 0;                                              /* key range */
        q[6] = 127;
        q[7] = 1;                                              /* velocity range */
        q[8] = 127;
        q[15] = 1;                                             /* sustain pedal */
    }
    return build_dump(0x49, 0, 0, perf, sizeof(perf));
}

/* The patch the SR's part 1 plays, Edisyn's way: select a performance (RAM 1 #0), send the test performance, the
 * patch to part 1 as usual, and the patch itself as a dump (changing patches is slow: the SR needs the pauses) */
typedef enum { S_PARAM, S_CC, S_PC, S_PERF, S_PATCH } step_kind_t;
typedef struct {
    uint8_t kind;
    uint16_t param;
    int16_t value;
    uint16_t gap_ms;
} step_t;
static step_t seq[24];
static int seq_n, seq_i;
#define STEP(k, p, v, g) (seq[seq_n++] = (step_t){k, p, v, g})

static void select_edit_patch(void)
{
    int bank = edit_bank(), num = synth_setting(SETTING_PATCH);
    seq_n = seq_i = 0;
    STEP(S_PARAM, PRM_MIDI_MODE, 1, 40);
    STEP(S_PARAM, PRM_MIDI_MODE, 0, 40);
    STEP(S_CC, 0, 0, 20);
    STEP(S_CC, 32, 0, 20);
    STEP(S_PC, 0, 0, 20);
    STEP(S_PARAM, PRM_CURRENT_BANK, 0, 40);
    STEP(S_PARAM, PRM_CURRENT_PROG, 0, 300);
    STEP(S_PERF, 0, 0, 300);
    STEP(S_PARAM, PRM_MIDI_MODE, 1, 40);
    STEP(S_PARAM, PRM_MIDI_MODE, 0, 40);
    STEP(S_PARAM, PRM_PART_PATCH_BANK, to_ws_bank[bank], 40);
    STEP(S_PARAM, PRM_PART_PATCH_NUM, num, 300);
    STEP(S_PARAM, PRM_MIDI_MODE, 1, 40);  /* changing the patch turns on OMNI: off again */
    STEP(S_PARAM, PRM_PERF_MIDI_MODE, 1, 40);
    if (synth_setting(SETTING_BULK)) {
        STEP(S_PATCH, 0, 0, 300);
    }
    sent_bank = bank;
    sent_num = num;
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
    case S_PERF:
        return midi_send_sysex(dump_buf, build_test_performance());
    default:
        return midi_send_sysex(dump_buf, build_patch_dump());
    }
}

static void mark_all_dirty(void)
{
    memset(dirty, 1, sizeof(dirty));
    name_dirty = true;
}

/* The whole patch to the SR: as a dump (then nothing is left to send) or value by value */
static void push_patch(void)
{
    if (synth_setting(SETTING_BULK)) {
        dump_pending = true;
        memset(dirty, 0, sizeof(dirty));
        name_dirty = false;
    } else {
        mark_all_dirty();
    }
}

static int field_param(int idx, int w)
{
    const field_t *f = &fields[idx];
    if (!(f->flags & F_WAVE) || (f->flags & F_NEEDW)) {
        return f->param;
    }
    return f->param + (idx >= F_BUSA && idx <= F_BUSD ? 4 : 7) * w;
}

static int field_message_value(int idx, int w)
{
    int v = get_field(idx, w);
    if (idx == F_BANK) {
        return to_ws_bank[v];
    }
    return fields[idx].kind == K_SLOPE ? v * 2 : v;
}

/* ---- bank dumps (ws_banks.c): IMPORT and REC > TO SR. Library numbers: bank * 35 + patch + 1 (1-420) ---- */
#define BANK_PATCHES 35
#define LIB_PATCHES (12 * BANK_PATCHES)
#define patch_kind ws_kind_patch
static const uint8_t writable_banks[4] = {0, 1, 2, 11};

static void tick(int64_t now)
{
    if (trail_key >= 0 && now > trail_until) {
        trail_key = -1;
        synth_redraw_page();
    }
    if (armed >= 0 && now > armed_until) {
        armed = -1;
        synth_redraw();
    }
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
            if (++seq_i == seq_n) {
                sent_wave = sent_point = -1;
                push_patch();  /* the SR plays its own patch now (or ours already): ours goes over it */
                dump_pending = false;
                if (!synth_setting(SETTING_BULK)) {
                    mark_all_dirty();
                }
            }
        }
        return;
    }
    int bank = synth_setting(SETTING_BANK), num = synth_setting(SETTING_PATCH);
    if (bank != sent_bank || num != sent_num) {
        select_edit_patch();
        return;
    }
    if (dump_pending) {
        if (midi_send_sysex(dump_buf, build_patch_dump())) {
            dump_pending = false;
            sent_wave = sent_point = -1;
            next_send_us = now + 300000;
        }
        return;
    }
    if (name_dirty) {
        char name[NAME_CHARS + 1];
        memcpy(name, patch, NAME_CHARS);
        name[NAME_CHARS] = 0;
        for (int i = NAME_CHARS - 1; i >= 0 && name[i] == ' '; i--) {
            name[i] = 0;
        }
        if (send_text(PRM_PART_PATCH_NAME, name)) {
            name_dirty = false;
            next_send_us = now + SEND_GAP_US;
        }
        return;
    }
    for (int idx = 0; idx < FIELD_COUNT; idx++) {
        const field_t *f = &fields[idx];
        int waves = (f->flags & F_WAVE) ? WAVES : 1;
        for (int w = 0; w < waves; w++) {
            if (!dirty[w][idx]) {
                continue;
            }
            if ((f->flags & F_WAVE) && !wave_active(w)) {
                dirty[w][idx] = false;  /* not used by the structure */
                continue;
            }
            /* the selections first, each its own message */
            if ((f->flags & F_NEEDW) && sent_wave != w) {
                if (send_param(PRM_CURRENT_WAVE, w)) {
                    sent_wave = w;
                    next_send_us = now + SEND_GAP_US;
                }
                return;
            }
            if (f->point >= 0 && sent_point != f->point) {
                if (send_param(PRM_MIX_ENV_POINT, f->point)) {
                    sent_point = f->point;
                    next_send_us = now + SEND_GAP_US;
                }
                return;
            }
            if (send_param(field_param(idx, w), field_message_value(idx, w))) {
                dirty[w][idx] = false;
                next_send_us = now + SEND_GAP_US;
            }
            return;
        }
    }
}

/* ---- names (kept in memory: reading a file while a knob is turned stalls the editor) ---- */

#define wave_name ws_wave_name

/* ---- envelope trail ----
 * While an envelope is being turned its earlier shape stays drawn dotted for a moment: the panel's own
 * afterglow only lingers on parts of the old line, this makes the whole old line linger evenly (as the Volca) */
#define TRAIL_REFRESH_US 350000  /* the trail follows at most this often */
#define TRAIL_HOLD_US 150000     /* and stays this long after the last change */

static int env_key(int idx, int w)
{
    return idx >= F_AE_L0 && idx <= F_AE_T4 ? w : idx >= F_E1_L0 && idx <= F_E1_T4 ? 4 + w : -1;
}

/* An envelope's points and times for the picture: amp (env 0) or envelope 1 (env 1) of wave w */
static void env_shape(int env, int w, int *lv, int *tm)
{
    static const int8_t amp[8] = {F_AE_L0, F_AE_T1, F_AE_L1, F_AE_T2, F_AE_L2, F_AE_T3, F_AE_L3, F_AE_T4};
    static const int8_t env1[8] = {F_E1_L0, F_E1_T1, F_E1_L1, F_E1_T2, F_E1_L2, F_E1_T3, F_E1_L3, F_E1_T4};
    const int8_t *f = env ? env1 : amp;
    for (int i = 0; i < 4; i++) {
        lv[i] = get_field(f[2 * i], w) * 100 / 99;
        tm[i + 1] = get_field(f[2 * i + 1], w) * 127 / 99;
    }
    tm[0] = 0;
    lv[4] = env ? get_field(F_E1_L4, w) * 100 / 99 : 0;  /* the amp envelope ends at 0 */
}

/* ---- editing ---- */

static int field_of(int p)
{
    if (p < FIELD_COUNT) {
        return p;
    }
    if (p == P_MIX_X) {
        return F_MIXX0 + point;
    }
    if (p == P_MIX_Y) {
        return F_MIXY0 + point;
    }
    if (p == P_MIX_TIME) {
        return point ? F_MIXT1 + point - 1 : -1;
    }
    return -1;
}

static void set_value(int idx, int v)
{
    const field_t *f = &fields[idx];
    int w = (f->flags & F_WAVE) ? cur_wave : 0;
    int key = env_key(idx, w);
    if (key >= 0) {
        int64_t now = esp_timer_get_time();
        if (key != trail_key || now - trail_taken > TRAIL_REFRESH_US) {
            env_shape(key / 4, w, trail_lv, trail_tm);  /* the shape before this change */
            trail_key = key;
            trail_taken = now;
        }
        trail_until = now + TRAIL_HOLD_US;
    }
    set_field(idx, w, v);
    dirty[w][idx] = true;
    if (idx == F_STRUCT) {
        for (int i = 0; i < FIELD_COUNT; i++) {
            if (fields[i].flags & F_WAVE) {
                for (int k = 0; k < WAVES; k++) {
                    dirty[k][i] = true;  /* waves that join in need everything */
                }
            }
        }
        if (!wave_active(cur_wave)) {
            cur_wave = 0;
        }
    }
}

static bool param_step(int p, int steps)
{
    if (p == P_POINT) {
        int np = point + steps;
        np = np < 0 ? 0 : np > 4 ? 4 : np;
        if (np == point) {
            return false;
        }
        point = np;
        synth_redraw();
        return true;
    }
    if (p == P_JOY_X || p == P_JOY_Y) {
        int *j = &joy[p == P_JOY_Y];
        int nj = *j + steps;
        nj = nj < 0 ? 0 : nj > 127 ? 127 : nj;
        if (nj == *j) {
            return false;
        }
        *j = nj;
        midi_send(0xB0 | channel(), p == P_JOY_X ? 16 : 17, nj);
        return true;
    }
    int idx = field_of(p);
    if (idx < 0) {
        return false;
    }
    int v = get_field(idx, fields[idx].flags & F_WAVE ? cur_wave : 0), nv = v + steps;
    nv = nv < fields[idx].min ? fields[idx].min : nv > fields[idx].max ? fields[idx].max : nv;
    if (nv == v) {
        return false;
    }
    set_value(idx, nv);
    return true;
}

/* The vector's values back to the centre (AC = BD = 0) and the times to the init patch's: the current point,
 * or all five. Loop, repeats and the mix modulation stay. */
static void reset_vector(bool all)
{
    ensure();
    for (int i = 0; i < 5; i++) {
        if (!all && i != point) {
            continue;
        }
        patch[66 + i] = patch[71 + i] = 127;
        dirty[0][F_MIXX0 + i] = dirty[0][F_MIXY0 + i] = true;
        if (i) {
            patch[15 + i] = init_patch[15 + i];
            dirty[0][F_MIXT1 + i - 1] = true;
        }
    }
    synth_redraw();
}

static void param_toggle(int p)
{
    if (p == P_RST_STEP || p == P_RST_VEC) {
        if (armed == p) {
            armed = -1;  /* the second press */
            reset_vector(p == P_RST_VEC);
        } else {
            armed = p;
            armed_until = esp_timer_get_time() + ARM_US;
            synth_redraw();
        }
        return;
    }
    int idx = field_of(p);
    if (idx >= 0) {
        set_value(idx, !get_field(idx, fields[idx].flags & F_WAVE ? cur_wave : 0));
    }
}

static bool param_reset(int p)
{
    if (p == P_JOY_X || p == P_JOY_Y) {
        return param_step(p, 64 - joy[p == P_JOY_Y]);
    }
    int idx = field_of(p);
    if (idx < 0) {
        return false;
    }
    ensure();
    const field_t *f = &fields[idx];
    uint8_t keep[PATCH_LEN];
    memcpy(keep, patch, PATCH_LEN);
    memcpy(patch, init_patch, PATCH_LEN);
    int v = get_field(idx, f->flags & F_WAVE ? cur_wave : 0);
    memcpy(patch, keep, PATCH_LEN);
    if (get_field(idx, f->flags & F_WAVE ? cur_wave : 0) == v) {
        return false;
    }
    set_value(idx, v);
    return true;
}

/* ---- formatting ---- */

static const char *param_name(int p)
{
    switch (p) {
    case P_POINT: return "POINT";
    case P_MIX_X: return "AC (X)";
    case P_MIX_Y: return "BD (Y)";
    case P_MIX_TIME: return "TIME";
    case P_JOY_X: return "JOY AC";
    case P_JOY_Y: return "JOY BD";
    case P_RST_STEP: return armed == p ? "SURE?" : "RST STEP";
    case P_RST_VEC: return armed == p ? "SURE?" : "RST VECT";
    default: return fields[p].name;
    }
}

static void param_format(int p, char *buf, size_t len)
{
    if (p == P_POINT) {
        snprintf(buf, len, "%d", point);
        return;
    }
    if (p == P_JOY_X || p == P_JOY_Y) {
        snprintf(buf, len, "%d", joy[p == P_JOY_Y]);
        return;
    }
    if (p == P_RST_STEP || p == P_RST_VEC) {
        buf[0] = 0;
        return;
    }
    int idx = field_of(p);
    if (idx < 0) {
        snprintf(buf, len, "--");  /* point 0 has no time */
        return;
    }
    const field_t *f = &fields[idx];
    int v = get_field(idx, f->flags & F_WAVE ? cur_wave : 0);
    switch (f->kind) {
    case K_SIGNED:
        snprintf(buf, len, v ? "%+d" : "0", v);
        break;
    case K_XY:
        snprintf(buf, len, v != 127 ? "%+d" : "0", v - 127);
        break;
    case K_SOURCE:
        snprintf(buf, len, "%s", source_names[v < 0 ? 0 : v > 12 ? 12 : v]);
        break;
    case K_SHAPE:
        snprintf(buf, len, "%s", shape_names[v < 0 ? 0 : v > 4 ? 4 : v]);
        break;
    case K_SWITCH:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    case K_BANK:
        snprintf(buf, len, "%s", bank_names[v < 0 ? 0 : v > 11 ? 11 : v]);
        break;
    case K_WAVE:
        snprintf(buf, len, v < 32 ? "WS %d" : "%d", v < 32 ? v : v - 32 + 1);
        break;
    case K_BEND:
        if (v < 0) {
            snprintf(buf, len, "GLOBAL");
        } else {
            snprintf(buf, len, v ? "%d" : "OFF", v);
        }
        break;
    case K_RAMPT:
        snprintf(buf, len, v == 100 ? "ON" : "%d", v);
        break;
    case K_REPEATS:
        snprintf(buf, len, v == 0 ? "OFF" : v == 127 ? "INF" : "%d", v);
        break;
    case K_LOOP:
        snprintf(buf, len, "%s", loop_names[v < 0 ? 0 : v > 6 ? 6 : v]);
        break;
    case K_STRUCT:
        snprintf(buf, len, "%s", structure_names[v < 0 ? 0 : v > 2 ? 2 : v]);
        break;
    case K_SLOPE:
        snprintf(buf, len, "%s%d.%02d", v < 0 ? "-" : "", (v < 0 ? -v : v) / 50, (v < 0 ? -v : v) % 50 * 2);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    if (p == P_POINT) {
        return point * 250;
    }
    if (p == P_JOY_X || p == P_JOY_Y) {
        return joy[p == P_JOY_Y] * 1000 / 127;
    }
    int idx = field_of(p);
    if (idx < 0) {
        return -1;  /* (also the reset buttons) */
    }
    const field_t *f = &fields[idx];
    if (f->kind == K_SWITCH || f->kind == K_SOURCE || f->kind == K_SHAPE || f->kind == K_BANK || f->kind == K_LOOP ||
        f->kind == K_STRUCT || f->kind == K_WAVE) {
        return -1;
    }
    return (get_field(idx, f->flags & F_WAVE ? cur_wave : 0) - f->min) * 1000 / (f->max - f->min);
}

static bool param_on(int p)
{
    if (p == P_RST_STEP || p == P_RST_VEC) {
        return armed == p;
    }
    int idx = field_of(p);
    return idx >= 0 && fields[idx].kind == K_SWITCH && get_field(idx, fields[idx].flags & F_WAVE ? cur_wave : 0);
}

static int param_units(int p)
{
    if (p == P_POINT) {
        return 60;
    }
    if (p == P_JOY_X || p == P_JOY_Y) {
        return 10;
    }
    int idx = field_of(p);
    if (idx < 0) {
        return 60;
    }
    const field_t *f = &fields[idx];
    if (f->kind == K_WAVE) {
        return 6;  /* among 518: fast turns accelerate */
    }
    int range = f->max - f->min;
    return range <= 1 ? 80 : range <= 6 ? 70 : range <= 16 ? 40 : range <= 30 ? 25 : 10;
}

/* The wave's number and name */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    if (p != F_WAVE_NUM) {
        return SYNTH_DRAW_NONE;
    }
    int v = get_field(F_WAVE_NUM, cur_wave);
    char code[8], name[WS_WAVE_NAME_LEN + 1];
    wave_name(get_field(F_BANK, cur_wave), v, name, sizeof(name));
    if (v < 32) {
        /* a wave sequence (bound to the bank): "WaveSeq" small before its number */
        snprintf(code, sizeof(code), "%d", v);
        jv880_draw_code_name(disp, x, y, w, "", name);
        int tw = display_text_width(&font_tiny, "WaveSeq"), nw = display_text_width(&font_medium, code);
        int x0 = x + (w - (tw + 3 + nw)) / 2;
        int base = y + 1 + font_medium.cap_top + font_medium.cap_height;
        display_text(disp, x0, base - font_tiny.cap_top - font_tiny.cap_height, &font_tiny, "WaveSeq", true);
        display_text(disp, x0 + tw + 3, y + 1, &font_medium, code, true);
        return SYNTH_DRAW_FULL;
    }
    snprintf(code, sizeof(code), "%d", v - 32 + 1);
    jv880_draw_code_name(disp, x, y, w, code, name);
    return SYNTH_DRAW_FULL;
}

/* ---- pictures ---- */

/* Envelope columns: "L0 50" at the bottom (the name's first and last character), the envelope over both
 * displays; the vector's column is the picture's, drawn by page_draw */
static bool column_draw(int p, int disp, int x)
{
    if (p == P_POINT) {
        return true;
    }
    if (!((p >= F_AE_L0 && p <= F_AE_T4) || (p >= F_E1_L0 && p <= F_E1_T4))) {
        return false;
    }
    char name[3] = {fields[p].name[0], fields[p].name[strlen(fields[p].name) - 1], 0}, val[8];
    param_format(p, val, sizeof(val));
    jv880_draw_eg_text(disp, x, name, val);
    return true;
}

/* The vector mix area. Structure 4 (waves A-D): a diamond (A left, B top, C right, D bottom) with the mix
 * envelope's path, points outside it clipped to its edge as the SR does. Structure 2 (A, C): only the AC axis
 * counts, so the path is drawn along it with each segment a row lower (a step down at every point). Structure
 * 1: there is no mix. The current point is the big square; its number ("PNT n") sits in the lower left corner. */
#define VEC_ROW_STEP 4  /* a segment's row below the one before, structure 2 */

static void draw_vector(void)
{
    const int cx = 33, cy = 34, r = 21;
    ensure();
    int structure = patch[86];
    display_fill_rect(0, 0, 12, 63, 52, false);
    if (structure == 0) {
        display_text(0, 11, 29, &font_small, "1 WAVE", true);
        display_text(0, 11, 41, &font_small, "NO MIX", true);
        return;
    }
    if (structure == 1) {
        /* the AC axis: A at -127, C at +127, a tick at the middle */
        display_line(0, 3, 43, 61, 43, true);
        display_line(0, 3, 40, 3, 46, true);
        display_line(0, 61, 40, 61, 46, true);
        display_line(0, 32, 41, 32, 45, true);
        display_text(0, 1, 47, &font_tiny, "A", true);
        display_text(0, 57, 47, &font_tiny, "C", true);
        int px = 0, y = 17;
        for (int i = 0; i < 5; i++) {
            int x = 3 + (patch[66 + i] - 127 + 127) * 58 / 254;
            if (i) {
                display_line(0, px, y, x, y, true);  /* along this segment's row */
                if (i < 4) {
                    display_line(0, x, y, x, y + VEC_ROW_STEP, true);  /* and down to the next */
                }
            }
            if (i == point) {
                display_fill_rect(0, x - 2, y - 2, 5, 5, true);
            } else {
                display_rect(0, x - 1, y - 1, 3, 3, true);
            }
            px = x;
            if (i && i < 4) {
                y += VEC_ROW_STEP;
            }
        }
    } else {
        display_line(0, cx - r, cy, cx, cy - r, true);
        display_line(0, cx, cy - r, cx + r, cy, true);
        display_line(0, cx + r, cy, cx, cy + r, true);
        display_line(0, cx, cy + r, cx - r, cy, true);
        display_text(0, 0, cy - 3, &font_tiny, "A", true);
        display_text(0, 58, cy - 3, &font_tiny, "C", true);
        display_text(0, cx + 4, 12, &font_tiny, "B", true);
        display_text(0, cx + 5, 56, &font_tiny, "D", true);
        int px = 0, py = 0;
        for (int i = 0; i < 5; i++) {
            int vx = patch[66 + i] - 127, vy = patch[71 + i] - 127;
            int mag = (vx < 0 ? -vx : vx) + (vy < 0 ? -vy : vy);
            if (mag > 127) {
                vx = vx * 127 / mag;
                vy = vy * 127 / mag;
            }
            int sx = cx + vx * r / 127, sy = cy - vy * r / 127;
            if (i) {
                display_line(0, px, py, sx, sy, true);
            }
            if (i == point) {
                display_fill_rect(0, sx - 2, sy - 2, 5, 5, true);
            } else {
                display_rect(0, sx - 1, sy - 1, 3, 3, true);
            }
            px = sx;
            py = sy;
        }
    }
    /* "PNT" and its number after it, along the bottom (the diamond's tip stops above) */
    char num[2] = {(char)('0' + point), 0};
    int tx = 3, ty = 56;
    int after = display_text(0, tx, ty, &font_tiny, "PNT", true) + 2;
    int base = ty + font_tiny.cap_top + font_tiny.cap_height;  /* the small digit stands on the same line */
    display_text(0, after, base - font_small.cap_top - font_small.cap_height, &font_small, num, true);
}

static void page_draw(int pg, int disp)
{
    if (pg == PG_VECTOR) {
        if (disp == 0) {
            draw_vector();
        }
        return;
    }
    if (pg != PG_AMP_ENV && pg != PG_ENV1) {
        return;
    }
    /* the third level is held while the key is; the amp envelope ends at 0 */
    int env = pg == PG_ENV1, lv[5], tm[5];
    env_shape(env, cur_wave, lv, tm);
    bool trail = trail_key == env * 4 + cur_wave;
    if (trail) {
        jv880_draw_env_ex(disp, trail_lv, trail_tm, 5, 3, false, true, true);
    }
    jv880_draw_env_ex(disp, lv, tm, 5, 3, false, false, !trail);
}

/* ---- patch overview (Grid) ----
 * Page 0: the four waves side by side (wave and its name, bank, level, tuning, cutoff, buses, the amp envelope),
 * page 1: the modulation, source by source - what each source does to the wave edited: its destinations and
 * amounts (the direct velocity and keyboard routes first, then the modulators with an amount). */

#define OV_HEADER_Y 12
#define OV_ROW_H 8
#define OV_ROWS 6  /* rows on one display (tiny font) below the header */
#define OV_COLS 50

static void ov_header(int disp, const char *left, const char *right)
{
    display_text(disp, 1, 0, &font_small, left, true);
    if (right) {
        int w = display_text_width(&font_small, right);
        display_text(disp, 254 - w, 0, &font_small, right, true);
    }
    display_fill_rect(disp, 0, 11, 255, 1, true);
}

static void ov_waves(int disp)
{
    ensure();
    char buf[48], name[WS_WAVE_NAME_LEN + 1];
    int s = patch[86];
    snprintf(buf, sizeof(buf), "OVERVIEW: waves  (structure %s)", structure_names[s > 2 ? 2 : s]);
    ov_header(disp, buf, NULL);
    if (disp == 1) {
        snprintf(buf, sizeof(buf), "%.15s", (const char *)patch);
        for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) {
            buf[i] = 0;
        }
        int w = display_text_width(&font_small, buf);
        display_fill_rect(1, 0, 0, 255, 11, false);
        display_text(1, 254 - w, 0, &font_small, buf, true);
        display_text(1, 1, 0, &font_small, "Grid: close", true);
    }
    for (int k = 0; k < 2; k++) {
        int wv = disp * 2 + k, x0 = k * 128;
        if (!wave_active(wv)) {
            snprintf(buf, sizeof(buf), "%c  (not used)", 'A' + wv);
            display_text(disp, x0 + 2, 13, &font_tiny, buf, true);
            continue;
        }
        int v = get_field(F_WAVE_NUM, wv), bank = get_field(F_BANK, wv);
        wave_name(bank, v, name, sizeof(name));
        snprintf(buf, sizeof(buf), v < 32 ? "%c WaveSeq %d %s" : "%c #%d %s", 'A' + wv, v < 32 ? v : v - 32 + 1, name);
        display_text(disp, x0 + 2, 13, &font_tiny, buf, true);
        snprintf(buf, sizeof(buf), "%s  LVL %d", bank_names[bank < 0 ? 0 : bank > 11 ? 11 : bank], get_field(F_LEVEL, wv));
        display_text(disp, x0 + 2, 21, &font_tiny, buf, true);
        snprintf(buf, sizeof(buf), "TUNE %+d/%+d  CUT %d", get_field(F_COARSE, wv), get_field(F_FINE, wv),
                 get_field(F_CUTOFF, wv));
        display_text(disp, x0 + 2, 29, &font_tiny, buf, true);
        snprintf(buf, sizeof(buf), "BUS %s%s%s%s%s", get_field(F_BUSA, wv) ? "A" : "", get_field(F_BUSB, wv) ? "B" : "",
                 get_field(F_BUSC, wv) ? "C" : "", get_field(F_BUSD, wv) ? "D" : "",
                 get_field(F_BUSA, wv) || get_field(F_BUSB, wv) || get_field(F_BUSC, wv) || get_field(F_BUSD, wv) ? "" : "-");
        display_text(disp, x0 + 2, 37, &font_tiny, buf, true);
        int lv[5], tm[5];
        env_shape(0, wv, lv, tm);
        jv880_draw_env_mini(disp, x0 + 3, 46, 120, 17, lv, tm, 5, 3);
    }
    display_fill_rect(disp, 127, 12, 1, 52, false);
    for (int y = 13; y < 63; y += 3) {
        display_pixel(disp, 127, y, true);  /* a dotted line between the two waves */
    }
}

/* ---- the modulation, source by source ---- */

typedef struct {
    uint8_t src;      /* 0-12 as the modulation sources; 13 = velocity (direct), 14 = keyboard (direct) */
    const char *dest;
    int16_t amount;
} mod_t;
#define MOD_MAX 40

static int collect_mods(mod_t *out)
{
    int n = 0, w = cur_wave;
    ensure();
#define ADD(sr, de, am)                                   \
    do {                                                  \
        int a_ = (am);                                    \
        if (a_ && n < MOD_MAX) {                          \
            out[n++] = (mod_t){(sr), (de), (int16_t)a_};  \
        }                                                 \
    } while (0)
    /* direct velocity and keyboard routes */
    ADD(13, "AMP LEVEL", get_field(F_AE_VELA, w));
    ADD(13, "AMP ATTACK", get_field(F_AE_VELR, w));
    ADD(13, "ENV1 LEVEL", get_field(F_E1_VELA, w));
    ADD(13, "ENV1 ATTACK", get_field(F_E1_VELR, w));
    ADD(13, "PAN", get_field(F_PAN_VEL, w));
    ADD(13, "PITCH RAMP", get_field(F_RAMP_VEL, w));
    ADD(14, "AMP DECAY", get_field(F_AE_KBD, w));
    ADD(14, "ENV1 RATE", get_field(F_E1_KBD, w));
    ADD(14, "PAN", get_field(F_PAN_KBD, w));
    ADD(14, "FILTER TRACK", get_field(F_FKBD, w));
    /* the modulation matrix: (source, amount) pairs */
    static const struct {
        int8_t src, amt;
        const char *dest;
    } matrix[] = {
        {F_PS1, F_PA1, "PITCH"}, {F_PS2, F_PA2, "PITCH"}, {F_AS1, F_AA1, "AMP"}, {F_AS2, F_AA2, "AMP"},
        {F_FS1, F_FA1, "FILTER"}, {F_FS2, F_FA2, "FILTER"}, {F_L1_RSRC, F_L1_RAMT, "LFO1 RATE"},
        {F_L1_DSRC, F_L1_DAMT, "LFO1 DEPTH"}, {F_L2_RSRC, F_L2_RAMT, "LFO2 RATE"},
        {F_L2_DSRC, F_L2_DAMT, "LFO2 DEPTH"},
    };
    for (size_t i = 0; i < sizeof(matrix) / sizeof(matrix[0]); i++) {
        ADD(get_field(matrix[i].src, w), matrix[i].dest, get_field(matrix[i].amt, w));
    }
    /* the vector mix (patch level): the AC axis from structure 2, BD from 4 */
    int st = patch[86];
    if (st >= 1) {
        ADD(get_field(F_XS1, 0), "MIX AC", get_field(F_XA1, 0));
        ADD(get_field(F_XS2, 0), "MIX AC", get_field(F_XA2, 0));
    }
    if (st >= 2) {
        ADD(get_field(F_YS1, 0), "MIX BD", get_field(F_YA1, 0));
        ADD(get_field(F_YS2, 0), "MIX BD", get_field(F_YA2, 0));
    }
#undef ADD
    return n;
}

#define ROWS_MAX 40
static char ov_rows[ROWS_MAX][OV_COLS + 2];
static int ov_row_n, ov_scroll;

/* A source's row: its name, then "DEST +amount" entries, continued on the next row when it's full */
static void build_rows(void)
{
    mod_t mods[MOD_MAX];
    int n = collect_mods(mods);
    ov_row_n = 0;
    /* order: velocity and keyboard (direct), then the matrix sources 0-12 */
    static const int8_t order[15] = {13, 14, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    for (int oi = 0; oi < 15; oi++) {
        int sr = order[oi];
        char line[OV_COLS + 2];
        int len = 0;
        bool first = true, any = false;
        const char *label = sr == 13 ? "VELOCITY" : sr == 14 ? "KEYBOARD" : source_names[sr];
        for (int k = 0; k < n; k++) {
            if (mods[k].src != sr) {
                continue;
            }
            char entry[24];
            snprintf(entry, sizeof(entry), "%s %+d", mods[k].dest, mods[k].amount);
            if (!any) {
                len = snprintf(line, sizeof(line), "%-8s ", label);
                any = true;
                first = true;
            } else if (len + 2 + (int)strlen(entry) > OV_COLS) {
                if (ov_row_n < ROWS_MAX) {
                    snprintf(ov_rows[ov_row_n++], sizeof(ov_rows[0]), "%s", line);
                }
                len = snprintf(line, sizeof(line), "         ");
                first = true;
            }
            len += snprintf(line + len, sizeof(line) - len, "%s%s", first ? "" : "  ", entry);
            first = false;
        }
        if (any && ov_row_n < ROWS_MAX) {
            snprintf(ov_rows[ov_row_n++], sizeof(ov_rows[0]), "%s", line);
        }
    }
}

static void ov_mods(int disp)
{
    build_rows();
    char buf[48];
    int max_scroll = ov_row_n > 2 * OV_ROWS ? ov_row_n - 2 * OV_ROWS : 0;
    if (ov_scroll > max_scroll) {
        ov_scroll = max_scroll;
    }
    snprintf(buf, sizeof(buf), "MODULATION  wave %c", 'A' + cur_wave);
    ov_header(disp, disp ? "source  destination amount" : buf, disp ? (max_scroll ? "encoder: scroll" : "Grid: close") : NULL);
    if (ov_row_n == 0 && disp == 0) {
        display_text(0, 2, 20, &font_tiny, "Nothing is modulated: set a SOURCE and an", true);
        display_text(0, 2, 28, &font_tiny, "AMOUNT on pages B-G (pitch, amp, filter,", true);
        display_text(0, 2, 36, &font_tiny, "LFO mods, mix mod).", true);
    }
    for (int r = 0; r < OV_ROWS; r++) {
        int idx = ov_scroll + disp * OV_ROWS + r;
        if (idx < ov_row_n) {
            display_text(disp, 1, 13 + r * OV_ROW_H, &font_tiny, ov_rows[idx], true);
        }
    }
}

static bool overview_scroll(int page, int steps)
{
    if (page != 1) {
        return false;
    }
    build_rows();
    int max_scroll = ov_row_n > 2 * OV_ROWS ? ov_row_n - 2 * OV_ROWS : 0;
    if (!max_scroll) {
        return false;  /* all of it is in view: the encoder turns the page */
    }
    ov_scroll += steps > 0 ? 1 : -1;
    ov_scroll = ov_scroll < 0 ? 0 : ov_scroll > max_scroll ? max_scroll : ov_scroll;
    return true;
}

static void overview_draw(int page, int disp)
{
    if (page == 0) {
        ov_waves(disp);
    } else {
        ov_mods(disp);
    }
}

/* ---- waves ---- */

static void select_wave(int wv)
{
    if (wv >= 0 && wv < WAVES && wave_active(wv)) {
        cur_wave = wv;
        synth_redraw();
    }
}

/* Step arrows: the next wave the structure uses, round */
static void wave_step(int d)
{
    ensure();
    int wv = cur_wave;
    for (int i = 0; i < WAVES; i++) {
        wv = (wv + (d > 0 ? 1 : WAVES - 1)) % WAVES;
        if (wave_active(wv)) {
            break;
        }
    }
    select_wave(wv);
}

static bool shift_group(int g)
{
    ensure();
    if (g >= WAVES) {
        return false;
    }
    select_wave(g);
    return true;
}

static int shift_group_led(int g)
{
    ensure();
    return g >= WAVES ? -1 : !wave_active(g) ? -1 : g == cur_wave;
}

static void header_tag(char *buf, size_t len) { snprintf(buf, len, "WAVE %c", 'A' + cur_wave); }

/* The pads play the SR's channel */
static int play_channel(void) { return channel(); }

static bool activate(void)
{
    ensure();
    select_edit_patch();
    return false;
}

/* ---- reading the unit's patch (Auto) ---- */

static void read_patch(void)
{
    int bank = synth_setting(SETTING_BANK), num = synth_setting(SETTING_PATCH);
    uint8_t msg[8] = {0xF0, 0x42, 0x30 + channel(), 0x28, 0x10, to_ws_bank[bank < 0 ? 0 : bank > 11 ? 11 : bank],
                      num, 0xF7};
    midi_send_sysex(msg, sizeof(msg));
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (ws_banks_sysex(d, len)) {
        return;  /* a bank dump or a status message: the job's */
    }
    /* F0 42 3n 28 40 bank num <852 nibbles> sum F7 */
    if (len != 861 || d[1] != 0x42 || d[3] != 0x28 || d[4] != 0x40) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    int sum = 0;
    for (int i = 7; i < 7 + 852; i++) {
        sum += d[i];
    }
    if ((sum & 0x7F) != d[len - 2]) {
        ESP_LOGW(TAG, "SysEx ignored: bad checksum");
        return;
    }
    ensure();
    for (int i = 0; i < PATCH_LEN; i++) {
        patch[i] = d[7 + 2 * i] | d[8 + 2 * i] << 4;
    }
    memset(dirty, 0, sizeof(dirty));  /* what was still to send is the unit's now */
    name_dirty = false;
    from_unit = true;
    memcpy(as_read, patch, PATCH_LEN);
    as_read_ok = true;
    synth_state_received();
    synth_sound_replaced();
    synth_redraw();
}

static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(patch, as_read, PATCH_LEN);
    push_patch();
    return true;
}

static bool sound_name(char *buf, size_t len)
{
    if (!from_unit) {
        return false;
    }
    snprintf(buf, len, "%.*s", NAME_CHARS, (const char *)patch);
    for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) {
        buf[i] = 0;
    }
    return true;
}

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
    push_patch();
    from_unit = as_read_ok = false;
    if (!wave_active(cur_wave)) {
        cur_wave = 0;
    }
    return true;
}

static bool patch_save(const char *path)
{
    ensure();
    /* The patch's own name (15 characters) follows the file's: "NNN Name" */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    for (int i = 0; i < NAME_LEN; i++) {
        char c = i < NAME_CHARS && *title ? *title++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
    }
    name_dirty = true;
    recompute_derived();
    mkdir(STORAGE_ROOT "/wsp", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    return ok;
}

/* ---- IMPORT and REC > TO SR ---- */

static void import_start(void)
{
    uint16_t mask;
    switch (synth_setting(SETTING_IMPORT) & 3) {
    case 0: mask = 0x0007; break;  /* RAM 1-3 */
    case 1: mask = 0x07F8; break;  /* ROM 4-11 */
    case 2: mask = 0x0800; break;  /* the card */
    default: mask = 1 << edit_bank(); break;
    }
    ws_banks_import(&patch_kind, mask, channel());
}

static void unit_slot_name(int slot, char *buf, size_t len)
{
    slot = slot < 0 ? 0 : slot > 4 * BANK_PATCHES - 1 ? 4 * BANK_PATCHES - 1 : slot;
    snprintf(buf, len, "%s:%02d", ws_bank_tags[writable_banks[slot / BANK_PATCHES]], slot % BANK_PATCHES);
}

/* What the slot holds, as far as the library's import of its bank knows */
static void unit_slot_info(int slot, char *buf, size_t len)
{
    snprintf(buf, len, "%.12s",
             ws_banks_lib_name(&patch_kind, writable_banks[(slot / BANK_PATCHES) & 3], slot % BANK_PATCHES));
}

/* The slot's bank is read, the patch put in it and the bank sent back */
static void unit_store(int slot, const char *name)
{
    if (ws_banks_busy()) {
        return;
    }
    ensure();
    for (int i = 0; i < NAME_LEN; i++) {
        char c = i < NAME_CHARS && *name ? *name++ : ' ';
        patch[i] = c >= 32 && c < 127 ? c : ' ';
    }
    name_dirty = true;
    recompute_derived();
    slot = slot < 0 ? 0 : slot > 4 * BANK_PATCHES - 1 ? 4 * BANK_PATCHES - 1 : slot;
    ws_banks_store(&patch_kind, writable_banks[slot / BANK_PATCHES], slot % BANK_PATCHES, patch, channel());
}

/* The header's label for an imported patch: its place in the SR instead of the library number */
static void patch_label(const char *file, char *buf, size_t len)
{
    int n = atoi(file);
    const char *sp = strchr(file, ' ');
    if (n < 1 || n > LIB_PATCHES || !sp) {
        snprintf(buf, len, "%s", file);
        return;
    }
    snprintf(buf, len, "%s:%02d%s", ws_bank_tags[(n - 1) / BANK_PATCHES], (n - 1) % BANK_PATCHES, sp);
}

static int unknown_count(void) { return 0; }

static const synth_tip_t tips[] = {
    {"WAVESTATION", {"A patch = up to four waves (A-D).", "Groups are functions; the wave is picked",
                     "with SHIFT + A-D or the STEP arrows", "(waves the structure doesn't use are",
                     "skipped). G MIX: the vector; the last", "two knobs play it live (CC 16, 17).",
                     "H PATCH: structure, hard sync."},
     {BTN_GROUP_A, BTN_GROUP_A + 6, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT, -1, -1, -1}},
    {"TO THE UNIT", {"A library patch goes to the SR as one", "dump, knob turns as single values. They", "edit the patch part 1 plays: EDIT BANK /",
                     "EDIT PATCH in CONTROL. AUTO reads it.", "Selecting the synth sends a test", "performance (RAM 1 #0, edit buffer):",
                     "never WRITE it on the SR."},
     {BTN_CONTROL, BTN_AUTO, -1, -1, -1, -1, -1, -1}},
    {"IMPORT, STORE", {"CONTROL > IMPORT (twice): the banks the", "IMPORT setting names, as library patches", "1-420 (bank x 35 + patch + 1).",
                       "REC > TO SR (twice): reads the bank, puts", "the patch in, sends the bank back.", "SR: PROTECT INT must be OFF.",
                       "Nothing works while it runs."},
     {BTN_CONTROL, BTN_REC, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_wsp = {
    .name = "Wavestation SR",
    .device = "Wavestation SR",
    .entry = "PATCH",
    .key = "wsp",
    .patch_dir = STORAGE_ROOT "/wsp",
    .default_patch = "000 Init",
    .patch_label = patch_label,
    .first_user_patch = 500,
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
    .page_draw = page_draw,
    .column_draw = column_draw,
    .overview_pages = 2,
    .overview_draw = overview_draw,
    .overview_scroll = overview_scroll,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .revert = revert,
    .unknown_count = unknown_count,
    .activate = activate,
    .play_channel = play_channel,
    .instance_step = wave_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_patch,
    .sysex_in = sysex_in,
    .tick = tick,
    .busy = ws_banks_busy,
    .busy_draw = ws_banks_draw,
    .action_name = "IMPORT",
    .action = import_start,
    .unit_store_label = "TO SR",
    .unit_store_slots = 4 * BANK_PATCHES,
    .unit_slot_name = unit_slot_name,
    .unit_slot_info = unit_slot_info,
    .unit_store = unit_store,
    .theme_bright = {127, 20, 20},  /* red */
    .theme_dim = {14, 2, 2},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
