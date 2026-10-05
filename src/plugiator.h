/*
 * Shared code for synth drivers of use-audio Plugiator plug-ins (MiniMax,
 * B4000, ...). Every parameter is a Control Change or a Polyphonic
 * Aftertouch message with the note number used as a parameter id; the
 * tables are generated per plug-in by tools/plugiator_import.py from the
 * Plug-in Manager's own files. Patch files are those messages, 3 bytes
 * each, channel nibble 0.
 *
 * A driver (synth_minimax.c, ...) owns one plug_set_t and forwards its
 * synth_t operations here, handling only its own virtual parameters.
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
    PT_STEPS,   /* positions: step_values[i] sent for position i, shown as
                   labels[i] (or 0-based numbers without labels) */
    PT_RAW24,   /* 0-24, shown as-is (bend range in semitones) */
    PT_RAW,     /* meaning of the value not known yet, 0-127 shown raw */
    PT_LIST,    /* value = index into labels, 0..label_count-1 */
    PT_MIX,     /* 0-127 crossfade between labels[0] and labels[1], shown as a ratio */
    PT_NOTE,    /* a key, 0-127, shown as a note name (0 = C-2) */
} plug_type_t;

typedef enum {
    PI_NONE,
    PI_WAVE,            /* Minimoog waveform, position 0-5: TRI ... NARROW */
    PI_DRAWBAR_BROWN,   /* organ drawbar, position 0-8, by handle colour */
    PI_DRAWBAR_WHITE,
    PI_DRAWBAR_BLACK,
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
    const char *const *labels;  /* PT_STEPS/PT_LIST position names, PT_MIX
                                   ends, or off/on names of a PT_SWITCH */
    uint8_t label_count;        /* PT_STEPS: also the number of positions */
    const uint8_t *step_values; /* PT_STEPS: value sent per position */
    plug_icon_t icon;           /* drawn instead of the value text */
    bool reverse;               /* MIDI value runs opposite to the knob */
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

/* One plug-in's parameter values and where its patches live. The editor
 * can't read the Plugiator's state, so values are "unknown" until a patch
 * is loaded or the parameter is touched. */
typedef struct {
    const char *tag;            /* log tag */
    const plug_param_t *params;
    int count;
    uint8_t *values, *init;     /* [count] each, provided by the driver */
    bool *known;                /* [count] */
    const char *patch_dir, *default_patch;
    bool initialised, init_read;
} plug_set_t;

const char *plug_name(plug_set_t *s, int p);
void plug_format(plug_set_t *s, int p, char *buf, size_t len);
int plug_bar(plug_set_t *s, int p);
bool plug_on(plug_set_t *s, int p);
int plug_units(plug_set_t *s, int p);
bool plug_step(plug_set_t *s, int p, int steps);
void plug_toggle(plug_set_t *s, int p);
int plug_draw(plug_set_t *s, int p, int disp, int x, int y, int w, int h);
bool plug_reset(plug_set_t *s, int p);
bool plug_patch_load(plug_set_t *s, const char *path);
bool plug_patch_save(plug_set_t *s, const char *path);
int plug_unknown_count(plug_set_t *s);
/* Loads the plug-in on the Plugiator: Program Change (the driver's
 * SETTING_PLUGIN_PC) + a confirming note - unless the editor loaded it
 * last (remembered across boots; assumes nobody else changes it); true if
 * it was sent */
bool plug_activate(plug_set_t *s);
/* The editor loaded this plug-in last */
bool plug_is_loaded(plug_set_t *s);

/* For drivers' virtual parameters */
void plug_init_values(plug_set_t *s);
void plug_set(plug_set_t *s, int p, uint8_t v);  /* store + mark known + send */
void plug_read_init_values(plug_set_t *s);
/* Position 0..label_count-1 of a PT_STEPS value: the nearest step */
int plug_step_pos(const plug_param_t *pp, uint8_t v);
