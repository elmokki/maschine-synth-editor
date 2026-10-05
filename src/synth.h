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

/* param_draw results */
#define SYNTH_DRAW_NONE 0   /* nothing drawn: the editor shows the value as text */
#define SYNTH_DRAW_LABEL 1  /* picture in the value row; formatted value small below */
#define SYNTH_DRAW_FULL 2   /* picture fills the whole area below the name */

/* Navigation is two-level, the same for every synth: Group buttons A-H pick
 * a section (e.g. an operator, tone or partial), the main encoder steps
 * through pages. Switching between parallel sections (same nonzero
 * `parallel` on their pages) keeps the page position, so OP1 page 2 -> OP2
 * page 2; any other section opens on the page it was last left on. */
typedef struct {
    uint8_t section;      /* 0-7 = Group A-H */
    const char *title;
    int16_t knobs[8];     /* parameter index per encoder, or SYNTH_NONE */
    int16_t switches[8];  /* parameter index per display button, or SYNTH_NONE */
    uint8_t parallel;     /* sections with the same nonzero value line up */
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

/* Every driver's settings start with the MIDI channel; the editor uses it
 * for the pads, the driver for everything it sends. (The pads' notes are
 * not per synth: nothing in the device's pads area is.) */
#define SETTING_CHANNEL 0
/* Plugiator drivers add: the Program Change that loads their plug-in. It
 * depends on the order plug-ins were installed on the box (tested here:
 * MiniMax 101, B4000 102, Lightwave 103 - the manual has B4000 and Lightwave
 * the other way round), so it's a setting. */
#define SETTING_PLUGIN_PC 1
#define SETTING_VOLUME 2
#define SETTING_SYNTH_MIC 3

/* A setting sent as a Control Change after activation */
typedef struct {
    int8_t setting;
    uint8_t cc;
} synth_device_cc_t;

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
    /* Optional: a picture instead of the value text, in the area below the
     * parameter name (x, y, w, h); returns a SYNTH_DRAW_* result */
    int (*param_draw)(int p, int disp, int x, int y, int w, int h);
    /* Back to the value it has in the default (init) patch, and send it;
     * false if that changed nothing */
    bool (*param_reset)(int p);

    /* Patches */
    bool (*patch_load)(const char *path);  /* read, send everything, update values */
    bool (*patch_save)(const char *path);
    int (*unknown_count)(void);            /* params a save would fill with defaults */

    /* Optional: make the instrument ready when this synth gets selected
     * (e.g. load a Plugiator plug-in); true = it had to change something
     * and the editor waits activate_ms before sending a patch, false = it
     * was ready already */
    bool (*activate)(void);
    int activate_ms;
    /* Optional: the instrument has this synth loaded right now (e.g. the
     * Plugiator's current plug-in), even when another synth is selected */
    bool (*is_loaded)(void);

    /* Optional device volume (Volume button + main encoder): sent as this
     * Control Change, kept in setting volume_setting and re-sent after
     * activate. volume_cc 0 = none. */
    uint8_t volume_cc;
    int8_t volume_setting;
    /* Optional: other device-level settings sent the same way after
     * activate, as (setting index, CC) pairs, terminated by cc 0 */
    const synth_device_cc_t *device_ccs;

    /* Colour theme, RGB 0-127, so each synth is recognisable at a glance:
     * bright = the selected section's Group button, dim = other sections
     * and the pads at rest. All zero = the editor's default blue. */
    uint8_t theme_bright[3], theme_dim[3];

    /* Optional: called often from the UI loop (e.g. to send throttled
     * updates) */
    void (*tick)(int64_t now_us);

    /* Optional: a picture over the whole page (e.g. an envelope across
     * every column, an algorithm diagram in empty columns), drawn after
     * the columns of display disp and redrawn whenever any column is. It
     * draws over (and must clear) only areas no column uses. */
    void (*page_draw)(int page, int disp);
    /* Optional: a section's own dim colour instead of the theme's (e.g.
     * carrier vs. modulator operators); false = theme colour */
    bool (*section_dim)(int section, uint8_t rgb[3]);
    /* Optional: draw parameter p's whole column below the switch row
     * instead of the editor's name/value/bar (e.g. small text under a
     * page-wide picture); false = the editor draws it */
    bool (*column_draw)(int p, int disp, int x);

    /* Optional: 8 patch category names (max 10 characters), one per
     * display button on the category screen (All button). A patch's
     * category is kept in <patch_dir>/categories: byte [patch number] =
     * 1-8, 0 = none. */
    const char *const *categories;
} synth_t;

/* Provided by the editor: the active synth's current setting value */
int synth_setting(int index);
/* Provided by the editor: re-run the active page's page_draw (e.g. when a
 * driver's picture changes by itself); does nothing outside the editor page */
void synth_redraw_page(void);

extern const synth_t synth_minimax;
extern const synth_t synth_b4000;
extern const synth_t synth_volca;
