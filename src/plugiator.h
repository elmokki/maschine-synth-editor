/*
 * Shared types for Plugiator plug-in parameter tables (generated per plug-in
 * by tools/plugiator_import.py, e.g. minimax_params.c).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    PLUG_CC,   /* Control Change, number = controller */
    PLUG_PAT,  /* Polyphonic Aftertouch, number = "note" used as parameter id */
} plug_msg_t;

typedef enum {
    PT_CONT,    /* 0-127 */
    PT_SWITCH,  /* 0 = off, 127 = on */
    PT_STEP6,   /* 6-position knob: 0, 22, 44, 66, 88, 110 */
    PT_RAW24,   /* 0-24, shown as-is (bend range in semitones) */
    PT_RAW,     /* meaning of the value not known yet, 0-127 shown raw */
    PT_LIST,    /* value = index into labels, 0..label_count-1 */
    PT_MIX,     /* 0-127 crossfade between labels[0] and labels[1], shown as a ratio */
} plug_type_t;

typedef enum {
    PI_NONE,
    PI_WAVE,    /* PT_STEP6 Minimoog waveform: TRI, SHARK, SAW, SQUARE, WIDE, NARROW */
} plug_icon_t;

/* A time knob's scale: (MIDI value, milliseconds) points, ascending */
typedef struct {
    uint8_t value;
    uint16_t ms;
} plug_curve_point_t;

typedef struct {
    const plug_curve_point_t *points;
    uint8_t count;
} plug_curve_t;

typedef struct {
    const char *short_name;     /* fits a 64 px display column */
    const char *name;           /* full name from config.xml */
    plug_msg_t msg;
    uint8_t number;
    plug_type_t type;
    uint8_t default_value;      /* most common value in the factory presets */
    bool in_preset;             /* false: device-level setting, not saved */
    uint8_t page;               /* 0 = Main panel, 1 = Add panel in the GUI */
    const char *const *labels;  /* PT_STEP6/PT_LIST position names, or off/on
                                   names of a PT_SWITCH; NULL = numbers / OFF-ON */
    uint8_t label_count;
    plug_icon_t icon;           /* drawn instead of the value text */
    /* Display scale (the plug-in GUI's knob legend), mapped linearly onto
     * 0-127; a range symmetric around 0 shows 64 as exactly 0. Both 0 =
     * show the raw value. */
    int16_t disp_lo, disp_hi;
    uint8_t decimals;           /* 0 or 1 */
    const char *unit;           /* appended to the scaled value, or NULL */
    const plug_curve_t *time_curve;  /* shown as a time instead, or NULL */
    /* Logarithmic display scale, as the GUI's number fields do it:
     * log_lo + (log_hi - log_lo) * (log_k^(v/127) - 1) / (log_k - 1);
     * log_k = 0 = not used */
    float log_lo, log_hi;
    uint16_t log_k;
} plug_param_t;
