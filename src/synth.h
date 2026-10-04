/*
 * Interface between the generic editor (editor.c: UI, navigation, patch
 * browsing/storing, settings) and one synth driver (e.g. synth_minimax.c:
 * parameters, pages, how values are sent and how patches are stored).
 *
 * A driver owns its parameter values; the editor only refers to
 * parameters by index and asks the driver to format, change and send them.
 * Everything runs in the USB client event loop, like the rest of the UI.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNTH_NONE (-1)

/* Navigation is two-level, the same for every synth: Group buttons A-H pick
 * a section (e.g. an operator, tone or partial), the main encoder steps
 * through pages. Switching between sections keeps the page position within
 * the section, so parallel sections (OP1 page 2 -> OP2 page 2) line up. */
typedef struct {
    uint8_t section;      /* 0-7 = Group A-H */
    const char *title;
    int16_t knobs[8];     /* parameter index per encoder, or SYNTH_NONE */
    int16_t switches[8];  /* parameter index per display button, or SYNTH_NONE */
} synth_page_t;

typedef enum {
    SET_NUMBER,   /* shown as is */
    SET_CHANNEL,  /* stored 0-15, shown 1-16 */
    SET_NOTE,     /* MIDI note number, shown as a note name */
} synth_setting_kind_t;

typedef struct {
    const char *name;  /* fits a 64 px display column */
    synth_setting_kind_t kind;
    int16_t min, max, def;
} synth_setting_t;

/* Every driver's settings start with these two; the editor uses them for
 * the pads, the driver for everything it sends */
#define SETTING_CHANNEL 0
#define SETTING_PAD_NOTE 1

typedef struct {
    const char *name;           /* shown in the UI, e.g. "MiniMax" */
    const char *key;            /* file names: settings, remembered patch */
    const char *patch_dir;      /* patch library directory on LittleFS */
    const char *default_patch;  /* loaded at boot if nothing is remembered */
    int first_user_patch;       /* lower numbers are factory, read-only */

    const synth_page_t *pages;
    int page_count;
    const synth_setting_t *settings;
    int setting_count;

    /* Parameters, by index */
    const char *(*param_name)(int p);
    void (*param_format)(int p, char *buf, size_t len);  /* "--" while unknown */
    int (*param_bar)(int p);              /* bar fill 0-1000, or -1 for no bar */
    bool (*param_on)(int p);              /* switch state for its box/LED */
    int (*param_units)(int p);            /* encoder counter units per step */
    bool (*param_step)(int p, int steps); /* change + send; false if unchanged */
    void (*param_toggle)(int p);          /* display button pressed; sends */
    /* Optional: draws a picture instead of the value text in the box; the
     * formatted value is then shown small underneath. NULL or false = text. */
    bool (*param_draw)(int p, int disp, int x, int y, int w, int h);

    /* Patches */
    bool (*patch_load)(const char *path);  /* read, send everything, update values */
    bool (*patch_save)(const char *path);
    int (*unknown_count)(void);            /* params a save would fill with defaults */
} synth_t;

/* Provided by the editor: the active synth's current setting value */
int synth_setting(int index);

extern const synth_t synth_minimax;
