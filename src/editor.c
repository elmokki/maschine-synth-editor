#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"
#include "fonts.h"
#include "maschine.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "textentry.h"
#include "editor.h"

static const char *TAG = "editor";

#define MAX_PATCHES 1000  /* file names are numbered 000-999 */
#define MAX_PATH 96

#define COLUMN_WIDTH 64
#define COLUMN_INNER 63  /* x=255 is cropped, keep every column 63 px wide */
#define NONE SYNTH_NONE

/* Encoder counter units per setting step (~1300 units per turn) */
#define UNITS_SETTING 65

#define BROWSE_TIMEOUT_US 4000000

/* Vertical layout, as the top row of capital letters of each line
 * (see draw_text_cap) */
#define HEADER_CAP_Y 1
#define HEADER_LINE_Y 11
#define SWITCH_BOX_Y 13
#define SWITCH_BOX_H 13
#define NAME_CAP_Y 28
#define VALUE_CAP_Y 39
#define BAR_Y 57
#define BAR_H 6

/* Button bits in report 0x01, see docs/PROTOCOL.md */
#define BTN_DISPLAY_1 0
#define BTN_CONTROL 8
#define BTN_BROWSE 10
#define BTN_ARROW_LEFT 12
#define BTN_ARROW_RIGHT 13
#define BTN_ARROW_LEFT_2 19
#define BTN_ARROW_RIGHT_2 20
#define BTN_ENTER 21
#define BTN_GROUP_A 24
#define BTN_REC 37
#define BTN_ERASE 38
#define BTN_MUTE 47
#define BTN_NAVIGATE 11  /* Sampling: next to Browse and Control */
#define BTN_SCENE 40     /* top of its column: the synth selection */
#define BTN_VOLUME 16    /* held: the main encoder sets the device volume */
#define VOLUME_STEP 4    /* per main encoder detent */

/* The last loaded patch is restored at boot, and settings persist. Both
 * are written once things have stayed unchanged this long, so browsing or
 * turning an encoder doesn't keep writing flash. */
#define STATE_DIR STORAGE_ROOT "/state"
#define SETTINGS_DIR STORAGE_ROOT "/settings"
#define WRITE_DELAY_US 5000000
#define MAX_SETTINGS 8

/* Mono button LEDs: every button that does something right now is lit
 * dimly, brightly when it's on / active */
#define LED_DIM 8
#define LED_ON 127

/* Group button LEDs, in the synth's colour theme: dim = section exists
 * (sliding to its complementary colour if it has several pages); the selected section is bright, and its
 * page shows on the button's left/right LEDs: page 1 = left, page 2 =
 * right, page 3 = whole button. A one-page section lights the whole
 * button. */
/* Default colour theme, for synths without their own (see synth_t) */
static const uint8_t default_bright[3] = {30, 90, 127}, default_dim[3] = {0, 4, 12};
/* A section with several pages: its dim colour as a brightness gradient
 * across the button's left/right LEDs (a second colour got muddy at these
 * levels and clashed with sections' own colours) */
#define MULTI_PAGE_GAIN_PCT 150
#define MULTI_PAGE_LOW_PCT 40
/* ...and the gradient slowly slides from side to side (0 = static) */
#define MULTI_PAGE_SLIDE_PERIOD_US 4000000
#define LED_ANIM_INTERVAL_US 50000
static int64_t led_anim_at;
#define LED_HALF_DIM 6  /* percent of full for the unlit half */

/* The synths to pick from (Scene), and the active one */
static const synth_t *const synths[] = {&synth_minimax, &synth_b4000, &synth_volca};
#define SYNTH_COUNT ((int)(sizeof(synths) / sizeof(synths[0])))
#define ACTIVE_SYNTH_FILE STORAGE_ROOT "/state/active_synth"
#define SYNTH_CONFIRM_US 3000000
static const synth_t *synth = &synth_minimax;
static bool usb_ready;              /* Maschine (and DIN MIDI) connected */
static int64_t activating_until;    /* synth being made ready, patch follows */
static int synth_confirm = -1;      /* synth column pressed once with unsaved edits */
static bool volume_held;
static int64_t synth_confirm_until;

static int encoder_acc[8];
static int page;
static uint8_t section_last[8];  /* page each section was last left on */
static bool overview;  /* section list shown while Navigate is held */
static bool encoder_browses;  /* Browse toggled: main encoder picks patches, not pages */

static char *patch_names[MAX_PATCHES];
static int patch_count;
static int patch_loaded = NONE;
static int patch_browse = NONE;  /* selection while browsing, not loaded yet */
static int64_t browse_deadline;
static bool modified;  /* the loaded patch has been edited since load/save */

static char last_saved[72];        /* contents of the remembered-patch file */
static int64_t remember_at;        /* when to write LAST_PATCH_FILE, 0 = nothing pending */
static bool restored;              /* last patch already restored since boot */

/* Erase: press = ask, press again = load the init patch; held while
 * turning an encoder = that parameter back to its init value */
#define INIT_CONFIRM_US 3000000
static bool erase_held, erase_used;
/* Rec opens the store screen on release, so Rec + Erase together (the
 * panel's "Replace") can revert the loaded patch to its stored state */
static bool rec_held, rec_used;
static int64_t init_armed_until;
/* Mute = panic (All Sound Off + All Notes Off); its LED flashes */
#define PANIC_FLASH_US 300000
static int64_t panic_flash_until;

static int16_t setting_values[MAX_SETTINGS];
static int64_t settings_save_at;   /* when to write the settings file, 0 = nothing pending */

/* Store (Rec), naming (pads as a phone keypad) and setup (Control) screens */
typedef enum { MODE_EDIT, MODE_STORE, MODE_NAME, MODE_SETUP, MODE_SYNTH } ui_mode_t;
static ui_mode_t mode;
static int store_target;  /* NONE = new patch, else index of a user patch */
static char store_name[TEXTENTRY_MAX_LEN + 1];
static int64_t delete_armed_until;  /* DELETE pressed once, press again to confirm */
#define DELETE_CONFIRM_US 3000000

static void load_patch_list(void);
static void draw_store(void);
static void draw_setup(void);
static void draw_synths(void);

int synth_setting(int index)
{
    return index < synth->setting_count ? setting_values[index] : 0;
}

/* "<dir>/<key><suffix>", e.g. /lfs/state/minimax_last */
static void synth_file(char *buf, size_t len, const char *dir, const char *suffix)
{
    snprintf(buf, len, "%s/%s%s", dir, synth->key, suffix);
}

/* ---- sections ---- */

static int section_first_page(int section)
{
    for (int i = 0; i < synth->page_count; i++) {
        if (synth->pages[i].section == section) {
            return i;
        }
    }
    return NONE;
}

static int section_page_count(int section)
{
    int n = 0;
    for (int i = 0; i < synth->page_count; i++) {
        n += synth->pages[i].section == section;
    }
    return n;
}

static bool has_theme(void)
{
    return synth->theme_bright[0] || synth->theme_bright[1] || synth->theme_bright[2];
}

static const uint8_t *theme_bright(void)
{
    return has_theme() ? synth->theme_bright : default_bright;
}

static const uint8_t *theme_dim(void)
{
    return has_theme() ? synth->theme_dim : default_dim;
}

/* Pads at rest, in the synth's theme */
static void pad_idle(int pad)
{
    const uint8_t *c = theme_dim();
    maschine_set_pad_led(pad, c[0], c[1], c[2]);
}

static void update_group_leds(void)
{
    for (int g = 0; g < 8; g++) {
        uint8_t own[3];
        bool own_dim = synth->section_dim && synth->section_dim(g, own);
        if (g == synth->pages[page].section) {
            int count = section_page_count(g);
            int k = page - section_first_page(g);
            bool lit_l = count == 1 || k != 1;
            bool lit_r = count == 1 || k >= 1;
            int left = lit_l ? 100 : LED_HALF_DIM;
            int right = lit_r ? 100 : LED_HALF_DIM;
            /* A section with its own colour brightens in that colour */
            uint8_t bright[3], l[3], r[3];
            memcpy(bright, theme_bright(), 3);
            int top = own[0] > own[1] ? (own[0] > own[2] ? own[0] : own[2])
                                      : (own[1] > own[2] ? own[1] : own[2]);
            if (own_dim && top) {
                for (int i = 0; i < 3; i++) {
                    bright[i] = own[i] * 127 / top;
                }
            }
            for (int i = 0; i < 3; i++) {
                l[i] = bright[i] * left / 100;
                r[i] = bright[i] * right / 100;
            }
            maschine_set_group_led_lr(g, l, r);
        } else if (section_page_count(g) > 1) {
            /* f: 0 = bright left / dim right, 1 = the other way round */
            float f = 0.0f;
            if (MULTI_PAGE_SLIDE_PERIOD_US) {
                int64_t t = esp_timer_get_time() % MULTI_PAGE_SLIDE_PERIOD_US;
                f = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * t / MULTI_PAGE_SLIDE_PERIOD_US);
            }
            const uint8_t *d = own_dim ? own : theme_dim();
            uint8_t l[3], r[3];
            for (int i = 0; i < 3; i++) {
                float c = d[i] * MULTI_PAGE_GAIN_PCT / 100.0f;
                float w = d[i] * MULTI_PAGE_LOW_PCT / 100.0f;
                l[i] = (uint8_t)(c + (w - c) * f + 0.5f);
                r[i] = (uint8_t)(w + (c - w) * f + 0.5f);
            }
            maschine_set_group_led_lr(g, l, r);
        } else if (section_first_page(g) != NONE) {
            const uint8_t *c = own_dim ? own : theme_dim();
            maschine_set_group_led(g, c[0], c[1], c[2]);
        } else {
            maschine_set_group_led(g, 0, 0, 0);
        }
    }
}

static void update_button_leds(void)
{
    /* Panic works on every screen */
    maschine_set_button_led(BTN_MUTE, panic_flash_until ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_CONTROL, mode == MODE_SETUP ? LED_ON : mode == MODE_EDIT ? LED_DIM : 0);
    maschine_set_button_led(BTN_SCENE, mode == MODE_SYNTH ? LED_ON : mode == MODE_EDIT ? LED_DIM : 0);
    if (mode == MODE_SETUP || mode == MODE_SYNTH) {
        for (int col = 0; col < 8; col++) {
            uint8_t v = 0;
            if (mode == MODE_SYNTH && col < SYNTH_COUNT) {
                v = synths[col] == synth || col == synth_confirm ? LED_ON : LED_DIM;
            }
            maschine_set_button_led(BTN_DISPLAY_1 + col, v);
        }
        static const int others[] = {BTN_ARROW_LEFT, BTN_ARROW_RIGHT, BTN_ARROW_LEFT_2,
                                     BTN_ARROW_RIGHT_2, BTN_NAVIGATE, BTN_ENTER, BTN_REC,
                                     BTN_BROWSE, BTN_ERASE};
        for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
            maschine_set_button_led(others[i], 0);
        }
        return;
    }
    if (mode != MODE_EDIT) {
        /* Only the soft keys drawn by draw_soft_keys() and Enter */
        bool store_keys[8] = {true, true, store_target != NONE, true};
        static const bool name_keys[8] = {true, false, false, true};
        for (int col = 0; col < 8; col++) {
            bool k = mode == MODE_STORE ? store_keys[col] : name_keys[col];
            maschine_set_button_led(BTN_DISPLAY_1 + col, k ? LED_DIM : 0);
        }
        maschine_set_button_led(BTN_ARROW_LEFT, 0);
        maschine_set_button_led(BTN_ARROW_RIGHT, 0);
        maschine_set_button_led(BTN_ARROW_LEFT_2, 0);
        maschine_set_button_led(BTN_ARROW_RIGHT_2, 0);
        maschine_set_button_led(BTN_NAVIGATE, 0);
        maschine_set_button_led(BTN_ENTER, LED_DIM);
        maschine_set_button_led(BTN_REC, LED_ON);
        maschine_set_button_led(BTN_BROWSE, 0);
        maschine_set_button_led(BTN_ERASE, 0);
        maschine_set_button_led(BTN_SCENE, 0);
        return;
    }
    maschine_set_button_led(BTN_REC, LED_DIM);
    maschine_set_button_led(BTN_BROWSE, encoder_browses ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_ERASE, init_armed_until || erase_held ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_VOLUME, !synth->volume_cc ? 0 : volume_held ? LED_ON : LED_DIM);
    for (int col = 0; col < 8; col++) {
        int sw = synth->pages[page].switches[col];
        uint8_t v = 0;
        if (sw != NONE) {
            v = synth->param_on(sw) ? LED_ON : LED_DIM;
        }
        maschine_set_button_led(BTN_DISPLAY_1 + col, v);
    }
    maschine_set_button_led(BTN_ARROW_LEFT, LED_DIM);
    maschine_set_button_led(BTN_ARROW_RIGHT, LED_DIM);
    maschine_set_button_led(BTN_ARROW_LEFT_2, LED_DIM);
    maschine_set_button_led(BTN_ARROW_RIGHT_2, LED_DIM);
    maschine_set_button_led(BTN_NAVIGATE, overview ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_ENTER, patch_browse != NONE && patch_browse != patch_loaded ? LED_ON : LED_DIM);
}

/* ---- drawing ---- */

/* Draws text so that the top of its capital letters lands on row cap_y */
static int draw_text_cap(int disp, int x, int cap_y, const font_t *font, const char *s, bool on)
{
    return display_text(disp, x, cap_y - font->cap_top, font, s, on);
}

static void draw_centered(int disp, int x, int w, int cap_y, const font_t *font,
                          const char *s, bool on)
{
    draw_text_cap(disp, x + (w - display_text_width(font, s)) / 2, cap_y, font, s, on);
}

static void draw_right(int disp, int right, int cap_y, const font_t *font, const char *s, bool on)
{
    draw_text_cap(disp, right - display_text_width(font, s), cap_y, font, s, on);
}

/* Copies s into buf, shortened with ".." until it fits max_px */
static void fit_text(const font_t *font, const char *s, int max_px, char *buf, size_t len)
{
    snprintf(buf, len, "%s", s);
    for (int n = strlen(s); n > 0 && display_text_width(font, buf) > max_px; n--) {
        snprintf(buf, len, "%.*s..", n - 1, s);
    }
}

/* Header row: left display = page title + patch name (or the patch being
 * browsed, inverted), right display = where the arrow buttons lead */
static void draw_header(void)
{
    if (overview || mode != MODE_EDIT) {
        return;
    }
    char buf[64];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, 0, DISPLAY_WIDTH, HEADER_LINE_Y + 1, false);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    if (init_armed_until) {
        /* The whole header asks, so the second press can't be mistaken */
        display_fill_rect(0, 0, 0, DISPLAY_USABLE_WIDTH, HEADER_LINE_Y, true);
        draw_text_cap(0, 2, HEADER_CAP_Y, &font_small, "PRESS ERASE AGAIN: INIT PATCH", false);
        draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "any other button cancels", true);
        return;
    }

    int first = section_first_page(synth->pages[page].section);
    int count = section_page_count(synth->pages[page].section);
    if (count > 1) {
        snprintf(buf, sizeof(buf), "%c  %s  %d/%d", 'A' + synth->pages[page].section,
                 synth->pages[page].title, page - first + 1, count);
    } else {
        snprintf(buf, sizeof(buf), "%c  %s", 'A' + synth->pages[page].section, synth->pages[page].title);
    }
    int title_end = draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, buf, true);

    int shown = patch_browse != NONE ? patch_browse : patch_loaded;
    /* While browsing, a patch other than the loaded one is shown inverted
     * with an Enter hint; landing back on the loaded one shows it plain */
    bool pending = patch_browse != NONE && patch_browse != patch_loaded;
    char name[64];
    if (shown == NONE) {
        snprintf(buf, sizeof(buf), "no patch loaded");
    } else {
        snprintf(buf, sizeof(buf), pending ? "%s  [ENTER]" : modified ? "%s *" : "%s",
                 patch_names[shown]);
    }
    int right = DISPLAY_USABLE_WIDTH - 2;
    fit_text(&font_small, buf, right - title_end - 10, name, sizeof(name));
    if (pending) {
        int w = display_text_width(&font_small, name);
        display_fill_rect(0, right - w - 2, 0, w + 4, HEADER_LINE_Y, true);
        draw_right(0, right, HEADER_CAP_Y, &font_small, name, false);
    } else {
        draw_right(0, right, HEADER_CAP_Y, &font_small, name, true);
    }

    if (volume_held) {
        snprintf(buf, sizeof(buf), "VOLUME %d", synth_setting(synth->volume_setting));
        display_fill_rect(1, 0, 0, DISPLAY_USABLE_WIDTH, HEADER_LINE_Y, true);
        draw_text_cap(1, 2, HEADER_CAP_Y, &font_small, buf, false);
    } else if (encoder_browses) {
        draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "BROWSE: encoder picks patch", true);
    } else {
        /* Where the main encoder leads */
        snprintf(buf, sizeof(buf), "< %s", synth->pages[(page + synth->page_count - 1) % synth->page_count].title);
        draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, buf, true);
        snprintf(buf, sizeof(buf), "%s >", synth->pages[(page + 1) % synth->page_count].title);
        draw_right(1, right, HEADER_CAP_Y, &font_small, buf, true);
    }
}

static void draw_column_only(int col)
{
    if (overview || mode != MODE_EDIT) {
        return;
    }
    int disp = col / 4;
    int x = (col % 4) * COLUMN_WIDTH;
    const synth_page_t *pg = &synth->pages[page];
    display_fill_rect(disp, x, HEADER_LINE_Y + 1, COLUMN_WIDTH,
                      DISPLAY_HEIGHT - HEADER_LINE_Y - 1, false);

    /* The switch on this column's display button */
    int sw = pg->switches[col];
    if (sw != NONE) {
        bool on = synth->param_on(sw);
        display_round_rect(disp, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, on, true);
        draw_centered(disp, x, COLUMN_INNER,
                      SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, synth->param_name(sw), !on);
    }

    /* The encoder's parameter: name, value, bar */
    int p = pg->knobs[col];
    if (p == NONE) {
        return;
    }
    if (synth->column_draw && synth->column_draw(p, disp, x)) {
        return;
    }
    draw_centered(disp, x, COLUMN_INNER, NAME_CAP_Y, &font_small, synth->param_name(p), true);

    char val[16];
    synth->param_format(p, val, sizeof(val));
    int drawn = synth->param_draw
                ? synth->param_draw(p, disp, x, VALUE_CAP_Y - 1, COLUMN_INNER,
                                    DISPLAY_HEIGHT - (VALUE_CAP_Y - 1))
                : SYNTH_DRAW_NONE;
    if (drawn == SYNTH_DRAW_LABEL) {
        /* Picture in the value row, the value's name below instead of a bar */
        draw_centered(disp, x, COLUMN_INNER, BAR_Y, &font_tiny, val, true);
        return;
    }
    if (drawn == SYNTH_DRAW_FULL) {
        return;
    }
    /* Largest font the value fits in, centred on the large font's caps */
    const font_t *vf = &font_large;
    if (display_text_width(vf, val) > COLUMN_INNER - 2) {
        vf = display_text_width(&font_medium, val) <= COLUMN_INNER - 2 ? &font_medium : &font_small;
    }
    draw_centered(disp, x, COLUMN_INNER,
                  VALUE_CAP_Y + (font_large.cap_height - vf->cap_height) / 2, vf, val, true);

    int bar = synth->param_bar(p);
    if (bar >= 0) {
        display_rect(disp, x + 4, BAR_Y, COLUMN_INNER - 8, BAR_H, true);
        display_fill_rect(disp, x + 6, BAR_Y + 2, (COLUMN_INNER - 12) * bar / 1000, BAR_H - 4, true);
    }
}

void synth_redraw_page(void)
{
    if (synth->page_draw && !overview && mode == MODE_EDIT && !activating_until) {
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            synth->page_draw(page, d);
        }
    }
}

static void draw_column(int col)
{
    draw_column_only(col);
    if (synth->page_draw && !overview && mode == MODE_EDIT) {
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            synth->page_draw(page, d);
        }
    }
}

/* Section list while Navigate is held: one 64 px cell per Group button,
 * A-D on the left display and E-H on the right, selected one inverted */
static void draw_overview(void)
{
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        draw_text_cap(d, 1, HEADER_CAP_Y, &font_small,
                      d == 0 ? "SECTIONS  (Group buttons)" : "release NAVIGATE to close", true);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    for (int g = 0; g < 8; g++) {
        int d = g / 4, x = (g % 4) * COLUMN_WIDTH;
        int first = section_first_page(g);
        bool sel = g == synth->pages[page].section;
        if (sel) {
            display_fill_rect(d, x + 1, HEADER_LINE_Y + 2, COLUMN_INNER - 2,
                              DISPLAY_HEIGHT - HEADER_LINE_Y - 2, true);
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%c", 'A' + g);
        draw_centered(d, x, COLUMN_INNER, 17, &font_large, buf, !sel);
        if (first == NONE) {
            draw_centered(d, x, COLUMN_INNER, 40, &font_small, "-", !sel);
            continue;
        }
        /* Long section names drop to the tiny font before being cut */
        const font_t *nf = display_text_width(&font_small, synth->pages[first].title) <= COLUMN_INNER - 4
                           ? &font_small : &font_tiny;
        char name[24];
        fit_text(nf, synth->pages[first].title, COLUMN_INNER - 4, name, sizeof(name));
        draw_centered(d, x, COLUMN_INNER, 40, nf, name, !sel);
        int count = section_page_count(g);
        if (count > 1) {
            snprintf(buf, sizeof(buf), "%d pages", count);
            draw_centered(d, x, COLUMN_INNER, 52, &font_small, buf, !sel);
        }
    }
}

/* While a synth is being made ready (plug-in loading): nothing else on the
 * displays, so it's clear edits would go nowhere yet */
static void draw_loading(void)
{
    char buf[40];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    snprintf(buf, sizeof(buf), "LOADING %s ...", synth->name);
    draw_centered(0, 0, DISPLAY_USABLE_WIDTH, 24, &font_large, buf, true);
    draw_centered(1, 0, DISPLAY_USABLE_WIDTH, 28, &font_small, "one moment", true);
}

static void draw_page(void)
{
    if (activating_until) {
        draw_loading();
        return;
    }
    if (mode == MODE_SETUP) {
        draw_setup();
        return;
    }
    if (mode == MODE_SYNTH) {
        draw_synths();
        return;
    }
    if (mode != MODE_EDIT) {
        draw_store();
        return;
    }
    if (overview) {
        draw_overview();
        return;
    }
    draw_header();
    for (int col = 0; col < 8; col++) {
        draw_column(col);
    }
}

static void end_browse(void)
{
    if (patch_browse != NONE) {
        patch_browse = NONE;
        draw_header();
    }
}

/* ---- patches ---- */

static void patch_path(char *buf, size_t len, const char *file)
{
    snprintf(buf, len, "%s/%s", synth->patch_dir, file);
}

static void load_patch(int idx)
{
    char path[MAX_PATH];
    patch_path(path, sizeof(path), patch_names[idx]);
    if (!synth->patch_load(path)) {
        return;
    }
    patch_loaded = idx;
    modified = false;
    remember_at = esp_timer_get_time() + WRITE_DELAY_US;
    ESP_LOGI(TAG, "loaded %s", patch_names[idx]);
}

/* ---- store / name ---- */

static int patch_number(int idx)
{
    return atoi(patch_names[idx]);
}

/* Patch name without the "NNN " number prefix of its file name */
static const char *patch_title(int idx)
{
    const char *sp = strchr(patch_names[idx], ' ');
    return sp ? sp + 1 : patch_names[idx];
}

static int next_free_number(void)
{
    int n = synth->first_user_patch;
    for (int i = 0; i < patch_count; i++) {
        if (patch_number(i) >= n) {
            n = patch_number(i) + 1;
        }
    }
    return n;
}

/* Steps through: new patch, then each user patch (factory ones are
 * read-only) */
static void set_store_target(int target)
{
    store_target = target;
    delete_armed_until = 0;
    if (target == NONE) {
        snprintf(store_name, sizeof(store_name), "User %d", next_free_number());
    } else {
        snprintf(store_name, sizeof(store_name), "%s", patch_title(target));
    }
}

/* Soft-key boxes on the left display's switch row, one per display button */
static void draw_soft_keys(const char *const *labels)
{
    for (int col = 0; col < 4; col++) {
        int x = col * COLUMN_WIDTH;
        display_fill_rect(0, x, SWITCH_BOX_Y, COLUMN_WIDTH, SWITCH_BOX_H, false);
        if (labels[col]) {
            display_round_rect(0, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, false, true);
            draw_centered(0, x, COLUMN_INNER,
                          SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                          &font_small, labels[col], true);
        }
    }
}

static void draw_store(void)
{
    char buf[64];
    display_clear(0);
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, mode == MODE_STORE ? "STORE PATCH" : "NAME PATCH", true);
    display_fill_rect(0, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);

    if (mode == MODE_NAME) {
        static const char *const keys[4] = {"OK", NULL, NULL, "CANCEL"};
        draw_soft_keys(keys);
        textentry_draw(SWITCH_BOX_Y + SWITCH_BOX_H + 1);
        return;
    }

    const char *keys[4] = {"SAVE", "RENAME", NULL, "CANCEL"};
    if (store_target != NONE) {
        keys[2] = delete_armed_until ? "SURE?" : "DELETE";
    }
    draw_soft_keys(keys);
    if (store_target == NONE) {
        snprintf(buf, sizeof(buf), "NEW  -  number %d", next_free_number());
    } else {
        snprintf(buf, sizeof(buf), "OVERWRITE  %d", patch_number(store_target));
    }
    draw_text_cap(0, 4, 31, &font_small, buf, true);
    char name[40];
    fit_text(&font_large, store_name, DISPLAY_USABLE_WIDTH - 8, name, sizeof(name));
    draw_text_cap(0, 4, 45, &font_large, name, true);

    display_clear(1);
    draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "Main encoder: new / overwrite", true);
    display_fill_rect(1, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    draw_text_cap(1, 4, 18, &font_small, "SAVE or ENTER stores the current sound.", true);
    draw_text_cap(1, 4, 31, &font_small, "Factory patches (000-099) are read-only.", true);
    if (store_target != NONE) {
        draw_text_cap(1, 4, 40, &font_small, "DELETE twice removes this patch.", true);
    }
    int unknown = synth->unknown_count();
    if (unknown) {
        snprintf(buf, sizeof(buf), "%d params never set - saved as defaults.", unknown);
        draw_text_cap(1, 4, 52, &font_small, buf, true);
    }
}

static void enter_store(void)
{
    end_browse();
    overview = false;
    mode = MODE_STORE;
    set_store_target(patch_loaded != NONE && patch_number(patch_loaded) >= synth->first_user_patch
                     ? patch_loaded : NONE);
    draw_page();
}

static void leave_store(void)
{
    textentry_stop();
    mode = MODE_EDIT;
    for (int pad = 0; pad < 16; pad++) {
        pad_idle(pad);
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    draw_page();
}

static void store_main_encoder(int steps)
{
    /* Targets: NONE (new), then the user patches in list order */
    static int targets[MAX_PATCHES + 1];
    int n = 0, cur = 0;
    targets[n++] = NONE;
    for (int i = 0; i < patch_count; i++) {
        if (patch_number(i) >= synth->first_user_patch) {
            if (i == store_target) {
                cur = n;
            }
            targets[n++] = i;
        }
    }
    set_store_target(targets[((cur + steps) % n + n) % n]);
    draw_store();
}

static void save_patch(void)
{
    int number = store_target == NONE ? next_free_number() : patch_number(store_target);
    char name[TEXTENTRY_MAX_LEN + 1];
    snprintf(name, sizeof(name), "%s", store_name[0] ? store_name : "Untitled");
    for (char *c = name; *c; c++) {
        if (*c == '/') {
            *c = '-';  /* not allowed in a file name */
        }
    }
    char file[TEXTENTRY_MAX_LEN + 8], path[MAX_PATH];
    snprintf(file, sizeof(file), "%03d %s", number, name);
    patch_path(path, sizeof(path), file);
    if (!synth->patch_save(path)) {
        ESP_LOGE(TAG, "writing %s failed", path);
        return;
    }
    if (store_target != NONE && strcmp(patch_names[store_target], file) != 0) {
        char old[MAX_PATH];
        patch_path(old, sizeof(old), patch_names[store_target]);
        remove(old);  /* renamed */
    }
    ESP_LOGI(TAG, "stored %s", file);

    load_patch_list();
    patch_loaded = NONE;
    for (int i = 0; i < patch_count; i++) {
        if (strcmp(patch_names[i], file) == 0) {
            patch_loaded = i;
            modified = false;
        }
    }
    remember_at = esp_timer_get_time() + WRITE_DELAY_US;
    leave_store();
}

static void delete_patch(void)
{
    char path[MAX_PATH];
    patch_path(path, sizeof(path), patch_names[store_target]);
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "deleting %s failed", path);
        return;
    }
    ESP_LOGI(TAG, "deleted %s", patch_names[store_target]);
    if (strcmp(patch_names[store_target], last_saved) == 0) {
        /* Don't try to restore a deleted patch at the next boot */
        char state[MAX_PATH];
        synth_file(state, sizeof(state), STATE_DIR, "_last");
        remove(state);
        last_saved[0] = '\0';
    }
    bool was_loaded = store_target == patch_loaded;
    char loaded_name[72] = "";
    if (patch_loaded != NONE && !was_loaded) {
        snprintf(loaded_name, sizeof(loaded_name), "%s", patch_names[patch_loaded]);
    }
    int deleted_idx = store_target;
    load_patch_list();
    /* Another patch was loaded: find it again, its index may have shifted */
    patch_loaded = NONE;
    for (int i = 0; i < patch_count && loaded_name[0]; i++) {
        if (strcmp(patch_names[i], loaded_name) == 0) {
            patch_loaded = i;
        }
    }
    /* The loaded one was deleted: load the patch before it in number order
     * (the one after it if it was the first), so there's always a patch */
    if (was_loaded && patch_count > 0) {
        int idx = deleted_idx > 0 ? deleted_idx - 1 : 0;
        load_patch(idx < patch_count ? idx : patch_count - 1);
    }
    leave_store();
}

static void store_button(int bit)
{
    int key = bit - BTN_DISPLAY_1;
    if (mode == MODE_STORE && key == 2 && store_target != NONE) {
        if (delete_armed_until) {
            delete_patch();
        } else {
            delete_armed_until = esp_timer_get_time() + DELETE_CONFIRM_US;
            draw_store();
        }
        return;
    }
    if (mode == MODE_STORE) {
        if (key == 0 || bit == BTN_ENTER) {
            save_patch();
        } else if (key == 1) {
            mode = MODE_NAME;
            textentry_start(store_name);
            draw_store();
        } else if (key == 3 || bit == BTN_REC) {
            leave_store();
        }
    } else {
        if (key == 0 || bit == BTN_ENTER) {
            snprintf(store_name, sizeof(store_name), "%s", textentry_text());
        }
        if (key == 0 || bit == BTN_ENTER || key == 3) {
            textentry_stop();
            mode = MODE_STORE;
            for (int pad = 0; pad < 16; pad++) {
                pad_idle(pad);
            }
            draw_store();
        }
    }
}

/* ---- setup (Control) ---- */

static void format_setting(int i, char *buf, size_t len)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F",
                                          "F#", "G", "G#", "A", "A#", "B"};
    int v = setting_values[i];
    switch (synth->settings[i].kind) {
    case SET_CHANNEL:
        snprintf(buf, len, "%d", v + 1);
        break;
    case SET_NOTE:  /* 60 = C3, so the default pad note 36 is C1 */
        snprintf(buf, len, "%s%d", names[v % 12], v / 12 - 2);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

/* Same column layout as the parameters: one setting per encoder */
static void draw_setup(void)
{
    char buf[48];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    snprintf(buf, sizeof(buf), "SETUP  %s", synth->name);
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, buf, true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "CONTROL closes", true);
    for (int i = 0; i < synth->setting_count && i < 8; i++) {
        int disp = i / 4, x = (i % 4) * COLUMN_WIDTH;
        draw_centered(disp, x, COLUMN_INNER, NAME_CAP_Y, &font_small, synth->settings[i].name, true);
        format_setting(i, buf, sizeof(buf));
        draw_centered(disp, x, COLUMN_INNER, VALUE_CAP_Y, &font_large, buf, true);
    }
}

static void setup_encoder(int i, int delta)
{
    if (i >= synth->setting_count) {
        return;
    }
    encoder_acc[i] += delta;
    int steps = encoder_acc[i] / UNITS_SETTING;
    if (steps == 0) {
        return;
    }
    encoder_acc[i] -= steps * UNITS_SETTING;
    const synth_setting_t *st = &synth->settings[i];
    int v = setting_values[i] + steps;
    v = v < st->min ? st->min : v > st->max ? st->max : v;
    if (v == setting_values[i]) {
        return;
    }
    setting_values[i] = v;
    settings_save_at = esp_timer_get_time() + WRITE_DELAY_US;
    draw_setup();
}

/* Settings file: one "NAME=value" line per setting, so adding settings to
 * a driver later doesn't invalidate saved files */
static void load_settings(void)
{
    for (int i = 0; i < synth->setting_count; i++) {
        setting_values[i] = synth->settings[i].def;
    }
    char path[MAX_PATH], line[48];
    synth_file(path, sizeof(path), SETTINGS_DIR, "");
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        for (int i = 0; i < synth->setting_count; i++) {
            const synth_setting_t *st = &synth->settings[i];
            int v = atoi(eq + 1);
            if (strcmp(line, st->name) == 0 && v >= st->min && v <= st->max) {
                setting_values[i] = v;
            }
        }
    }
    fclose(f);
}

static void save_settings(void)
{
    char path[MAX_PATH];
    synth_file(path, sizeof(path), SETTINGS_DIR, "");
    mkdir(SETTINGS_DIR, 0777);  /* fails harmlessly if it exists */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "can't write %s", path);
        return;
    }
    for (int i = 0; i < synth->setting_count; i++) {
        fprintf(f, "%s=%d\n", synth->settings[i].name, setting_values[i]);
    }
    fclose(f);
    ESP_LOGI(TAG, "saved settings %s", path);
}

/* ---- init / panic ---- */

/* First edit of the loaded patch: show the unsaved-changes mark */
static void mark_modified(void)
{
    if (!modified) {
        modified = true;
        draw_header();
    }
}

static void cancel_init(void)
{
    if (init_armed_until) {
        init_armed_until = 0;
        draw_header();
    }
}

static void init_patch(void)
{
    init_armed_until = 0;
    for (int i = 0; i < patch_count; i++) {
        if (strcmp(patch_names[i], synth->default_patch) == 0) {
            end_browse();
            load_patch(i);
            draw_page();
            return;
        }
    }
    ESP_LOGW(TAG, "init patch %s not found", synth->default_patch);
    draw_header();
}

/* Rec + Erase: back to the loaded patch as stored, dropping edits */
static void revert_patch(void)
{
    rec_used = erase_used = true;
    if (patch_loaded == NONE || !modified) {
        return;  /* nothing to revert */
    }
    ESP_LOGI(TAG, "reverting %s", patch_names[patch_loaded]);
    load_patch(patch_loaded);
    draw_page();
}

static void panic(void)
{
    int ch = synth_setting(SETTING_CHANNEL);
    midi_send(0xB0 | ch, 120, 0);  /* All Sound Off */
    midi_send(0xB0 | ch, 123, 0);  /* All Notes Off */
    panic_flash_until = esp_timer_get_time() + PANIC_FLASH_US;
    ESP_LOGI(TAG, "panic on channel %d", ch + 1);
}

/* ---- synth selection (Scene) ---- */

static void load_settings(void);

/* One column per synth, its display button picks it; the active one is
 * filled, one asking for confirmation says SURE? */
static void draw_synths(void)
{
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, "SYNTH", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "SCENE closes", true);
    for (int i = 0; i < SYNTH_COUNT && i < 8; i++) {
        int disp = i / 4, x = (i % 4) * COLUMN_WIDTH;
        bool active = synths[i] == synth;
        const char *label = i == synth_confirm ? "SURE?" : synths[i]->name;
        display_round_rect(disp, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, active, true);
        draw_centered(disp, x, COLUMN_INNER, SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, label, !active);
        if (active) {
            draw_centered(disp, x, COLUMN_INNER, NAME_CAP_Y, &font_small, "ACTIVE", true);
        }
    }
    if (synth_confirm >= 0) {
        draw_text_cap(1, 4, 30, &font_small, "The patch has unsaved edits.", true);
        draw_text_cap(1, 4, 44, &font_small, "Press again to switch anyway.", true);
    }
}

/* Patch to bring the synth back to: the remembered one, or the default */
static void send_volume(void)
{
    if (synth->volume_cc) {
        midi_send(0xB0 | synth_setting(SETTING_CHANNEL), synth->volume_cc,
                  synth_setting(synth->volume_setting));
    }
}

/* Device-level settings that aren't part of patches, after activation */
static void send_device_settings(void)
{
    send_volume();
    for (int i = 0; synth->device_ccs && synth->device_ccs[i].cc; i++) {
        midi_send(0xB0 | synth_setting(SETTING_CHANNEL), synth->device_ccs[i].cc,
                  synth_setting(synth->device_ccs[i].setting));
    }
}

static void restore_patch(void)
{
    send_device_settings();
    const char *candidates[2] = {last_saved, synth->default_patch};
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < patch_count && candidates[c][0]; i++) {
            if (strcmp(patch_names[i], candidates[c]) == 0) {
                load_patch(i);
                remember_at = 0;  /* nothing new to remember */
                draw_page();
                return;
            }
        }
        if (c == 0 && last_saved[0]) {
            ESP_LOGW(TAG, "remembered patch %s not found, loading the default", last_saved);
        }
    }
    ESP_LOGW(TAG, "default patch %s not found either", synth->default_patch);
    draw_page();
}

/* Make the instrument ready (e.g. load the Plugiator plug-in), then send
 * the patch once it has had time to switch */
static void start_activation(void)
{
    if (!usb_ready) {
        return;  /* editor_connected() does it */
    }
    if (synth->activate) {
        synth->activate();
        activating_until = esp_timer_get_time() + synth->activate_ms * 1000LL;
        draw_page();
    } else {
        restore_patch();
    }
}

static void read_last_patch(void)
{
    char path[MAX_PATH];
    last_saved[0] = '\0';
    synth_file(path, sizeof(path), STATE_DIR, "_last");
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(last_saved, sizeof(last_saved), f)) {
            last_saved[0] = '\0';
        }
        fclose(f);
    }
}

static void switch_synth(int i)
{
    synth = synths[i];
    ESP_LOGI(TAG, "synth: %s", synth->name);
    mkdir(STATE_DIR, 0777);
    FILE *f = fopen(ACTIVE_SYNTH_FILE, "w");
    if (f) {
        fputs(synth->key, f);
        fclose(f);
    }
    /* Everything the editor holds about the previous synth goes */
    page = 0;
    memset(section_last, 0, sizeof(section_last));
    memset(encoder_acc, 0, sizeof(encoder_acc));
    patch_loaded = patch_browse = NONE;
    modified = false;
    remember_at = settings_save_at = 0;
    load_settings();
    load_patch_list();
    read_last_patch();
    mode = MODE_EDIT;
    synth_confirm = -1;
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    for (int pad = 0; pad < 16; pad++) {
        pad_idle(pad);  /* the new synth's colours */
    }
    draw_page();
    update_group_leds();
    start_activation();
}

static void pick_synth(int i)
{
    if (synths[i] == synth) {
        mode = MODE_EDIT;  /* the active one: just close */
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        return;
    }
    if (modified && synth_confirm != i) {
        synth_confirm = i;
        synth_confirm_until = esp_timer_get_time() + SYNTH_CONFIRM_US;
        draw_synths();
        return;
    }
    switch_synth(i);
}

/* ---- input ---- */

static void setup_encoder(int i, int delta);

static void editor_encoder_impl(int i, int delta)
{
    if (activating_until) {
        return;  /* the synth isn't listening yet */
    }
    if (mode == MODE_SETUP) {
        setup_encoder(i, delta);
        return;
    }
    if (mode != MODE_EDIT) {
        return;
    }
    end_browse();
    cancel_init();
    int p = synth->pages[page].knobs[i];
    if (p == NONE) {
        return;
    }
    if (erase_held) {
        erase_used = true;  /* releasing Erase now doesn't ask for a patch init */
        encoder_acc[i] = 0;
        if (synth->param_reset && synth->param_reset(p)) {
            draw_column(i);
            mark_modified();
        }
        return;
    }
    int units = synth->param_units(p);
    encoder_acc[i] += delta;
    int steps = encoder_acc[i] / units;
    if (steps == 0) {
        return;
    }
    encoder_acc[i] -= steps * units;
    if (synth->param_step(p, steps)) {
        draw_column(i);
        mark_modified();
    }
}

static void go_to_page(int new_page)
{
    page = new_page;
    int section = synth->pages[page].section;
    section_last[section] = page - section_first_page(section);
    memset(encoder_acc, 0, sizeof(encoder_acc));
    draw_page();
    update_group_leds();
}

static void change_page(int d)
{
    go_to_page((page + d + synth->page_count) % synth->page_count);
}

/* Steps the patch shown (inverted) in the header; Enter loads it */
static void browse_patches(int steps)
{
    if (patch_count == 0) {
        return;
    }
    int from = patch_browse != NONE ? patch_browse : patch_loaded != NONE ? patch_loaded : -1;
    int idx = from + steps;
    patch_browse = (idx % patch_count + patch_count) % patch_count;
    browse_deadline = esp_timer_get_time() + BROWSE_TIMEOUT_US;
    draw_header();
}

static void select_section(int section)
{
    int first = section_first_page(section);
    if (first == NONE) {
        return;
    }
    int count = section_page_count(section);
    if (section == synth->pages[page].section) {
        /* Already here: step to the section's next page */
        go_to_page(first + (page - first + 1) % count);
        return;
    }
    /* Parallel sections: keep the position within the section, clamped to
     * its length; others: back where that section was left */
    int offset = section_last[section];
    uint8_t par = synth->pages[page].parallel;
    if (par && synth->pages[first].parallel == par) {
        offset = page - section_first_page(synth->pages[page].section);
    }
    go_to_page(first + (offset < count ? offset : count - 1));
}

static void editor_button_impl(int bit, bool down)
{
    if (bit == BTN_MUTE) {
        if (down) {
            panic();
        }
        return;
    }
    if (activating_until) {
        return;  /* the synth isn't listening yet */
    }
    if (bit == BTN_SCENE && down && (mode == MODE_EDIT || mode == MODE_SYNTH)) {
        end_browse();
        cancel_init();
        overview = false;
        synth_confirm = -1;
        mode = mode == MODE_SYNTH ? MODE_EDIT : MODE_SYNTH;
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        return;
    }
    if (mode == MODE_SYNTH) {
        if (down && bit >= BTN_DISPLAY_1 && bit < BTN_DISPLAY_1 + SYNTH_COUNT) {
            pick_synth(bit - BTN_DISPLAY_1);
        }
        return;
    }
    if (bit == BTN_CONTROL && down && (mode == MODE_EDIT || mode == MODE_SETUP)) {
        end_browse();
        overview = false;
        mode = mode == MODE_SETUP ? MODE_EDIT : MODE_SETUP;
        memset(encoder_acc, 0, sizeof(encoder_acc));
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        return;
    }
    if (mode == MODE_SETUP) {
        return;
    }
    if (mode != MODE_EDIT) {
        if (down) {
            store_button(bit);
        }
        return;
    }
    if (bit == BTN_VOLUME && synth->volume_cc) {
        volume_held = down;
        if (down) {
            end_browse();
            cancel_init();
        }
        draw_header();
        return;
    }
    if (bit == BTN_REC) {
        if (down) {
            cancel_init();
            rec_held = true;
            rec_used = false;
            if (erase_held) {
                revert_patch();
            }
        } else if (rec_held) {
            rec_held = false;
            if (!rec_used) {
                enter_store();
            }
        }
        return;
    }
    if (bit == BTN_ERASE) {
        if (down) {
            erase_held = true;
            erase_used = false;
            if (rec_held) {
                revert_patch();
            }
        } else {
            erase_held = false;
            if (!erase_used) {
                if (init_armed_until) {
                    init_patch();
                } else {
                    end_browse();
                    init_armed_until = esp_timer_get_time() + INIT_CONFIRM_US;
                    draw_header();
                }
            }
        }
        return;
    }
    if (down) {
        cancel_init();  /* any other button */
    }
    if (bit == BTN_NAVIGATE) {
        overview = down;
        draw_page();
        return;
    }
    if (!down) {
        return;
    }
    if (bit >= BTN_DISPLAY_1 && bit < BTN_DISPLAY_1 + 8) {
        int col = bit - BTN_DISPLAY_1;
        int sw = synth->pages[page].switches[col];
        if (sw != NONE) {
            synth->param_toggle(sw);
            mark_modified();
            /* A switch can affect others on the page (e.g. radio buttons),
             * so redraw them all; only changed chunks go out */
            for (int c = 0; c < 8; c++) {
                draw_column(c);
            }
        }
    } else if (bit == BTN_ARROW_LEFT) {
        change_page(-1);  /* top arrows, by the display buttons: pages */
    } else if (bit == BTN_ARROW_RIGHT) {
        change_page(1);
    } else if (bit == BTN_ARROW_LEFT_2) {
        browse_patches(-1);  /* arrows under the main encoder, by Enter: patches */
    } else if (bit == BTN_ARROW_RIGHT_2) {
        browse_patches(1);
    } else if (bit == BTN_BROWSE) {
        encoder_browses = !encoder_browses;
        draw_header();
    } else if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8) {
        select_section(bit - BTN_GROUP_A);
    } else if (bit == BTN_ENTER && patch_browse != NONE) {
        load_patch(patch_browse);
        patch_browse = NONE;
        draw_page();
    }
}

static void editor_main_encoder_impl(int steps)
{
    if (activating_until) {
        return;
    }
    if (volume_held && mode == MODE_EDIT) {
        int v = setting_values[synth->volume_setting] + steps * VOLUME_STEP;
        setting_values[synth->volume_setting] = v < 0 ? 0 : v > 127 ? 127 : v;
        settings_save_at = esp_timer_get_time() + WRITE_DELAY_US;
        send_volume();
        draw_header();
        return;
    }
    cancel_init();
    if (mode == MODE_STORE) {
        store_main_encoder(steps);
        return;
    }
    if (mode != MODE_EDIT) {
        return;
    }
    if (encoder_browses) {
        browse_patches(steps);
    } else {
        change_page(steps);
    }
}

static void remember_loaded_patch(void)
{
    if (patch_loaded == NONE || strcmp(patch_names[patch_loaded], last_saved) == 0) {
        return;
    }
    char path[MAX_PATH];
    synth_file(path, sizeof(path), STATE_DIR, "_last");
    mkdir(STATE_DIR, 0777);  /* fails harmlessly if it exists */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "can't write %s", path);
        return;
    }
    fputs(patch_names[patch_loaded], f);
    fclose(f);
    snprintf(last_saved, sizeof(last_saved), "%s", patch_names[patch_loaded]);
    ESP_LOGI(TAG, "remembered %s for next boot", last_saved);
}

static void save_settings(void);

static void editor_tick_impl(int64_t now_us)
{
    textentry_tick(now_us);
    if (synth->tick) {
        synth->tick(now_us);
    }
    if (MULTI_PAGE_SLIDE_PERIOD_US && now_us > led_anim_at) {
        led_anim_at = now_us + LED_ANIM_INTERVAL_US;
        update_group_leds();
    }
    if (activating_until && now_us > activating_until) {
        activating_until = 0;
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        restore_patch();
    }
    if (synth_confirm >= 0 && now_us > synth_confirm_until) {
        synth_confirm = -1;
        if (mode == MODE_SYNTH) {
            draw_synths();
        }
    }
    if (init_armed_until && now_us > init_armed_until) {
        cancel_init();
    }
    if (panic_flash_until && now_us > panic_flash_until) {
        panic_flash_until = 0;
    }
    if (settings_save_at && now_us > settings_save_at) {
        settings_save_at = 0;
        save_settings();
    }
    if (remember_at && now_us > remember_at) {
        remember_at = 0;
        remember_loaded_patch();
    }
    if (delete_armed_until && now_us > delete_armed_until) {
        delete_armed_until = 0;  /* not confirmed */
        if (mode == MODE_STORE) {
            draw_store();
        }
    }
    if (patch_browse != NONE && now_us > browse_deadline) {
        end_browse();  /* browsed away without loading */
    }
}

static void editor_redraw_impl(void)
{
    draw_page();
    update_group_leds();
}

/* ---- setup ---- */

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_patch_list(void)
{
    for (int i = 0; i < patch_count; i++) {
        free(patch_names[i]);
    }
    patch_count = 0;
    DIR *dir = opendir(synth->patch_dir);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) && patch_count < MAX_PATCHES) {
            patch_names[patch_count++] = strdup(e->d_name);
        }
        closedir(dir);
        qsort(patch_names, patch_count, sizeof(char *), cmp_names);
    }
    ESP_LOGI(TAG, "%d %s patches in %s", patch_count, synth->name, synth->patch_dir);
}

static void load_settings(void);

static void editor_init_impl(void)
{
    char key[24] = "";
    FILE *f = fopen(ACTIVE_SYNTH_FILE, "r");
    if (f) {
        if (!fgets(key, sizeof(key), f)) {
            key[0] = '\0';
        }
        fclose(f);
    }
    for (int i = 0; i < SYNTH_COUNT; i++) {
        if (strcmp(synths[i]->key, key) == 0) {
            synth = synths[i];
        }
    }
    ESP_LOGI(TAG, "synth: %s", synth->name);
    load_settings();
    load_patch_list();
    read_last_patch();
    for (int pad = 0; pad < 16; pad++) {
        pad_idle(pad);
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    draw_page();
    update_group_leds();
}

/* First connection since boot: make the synth ready and bring it back to
 * the patch that was loaded last (or its default patch), so the editor's
 * values are known */
static void editor_connected_impl(void)
{
    if (restored) {
        return;
    }
    restored = true;
    usb_ready = true;
    start_activation();
}

/* Pads play notes on the synth's channel, or type in the name screen */
static void editor_pad_impl(int pad, bool down, int raw)
{
    if (activating_until && down) {
        return;  /* a note now could be taken as the plug-in's confirm note */
    }
    if (mode == MODE_NAME) {
        if (down) {
            textentry_pad(pad, esp_timer_get_time());
        }
        return;
    }
    /* Pad 12 (bottom left) is the lowest note, rows go up like a keyboard */
    int note = synth_setting(SETTING_PAD_NOTE) + (3 - pad / 4) * 4 + pad % 4;
    int channel = synth_setting(SETTING_CHANNEL);
    if (down) {
        /* Velocity from the hit's peak pressure (main.c), over the range
         * from the press threshold (~1000) to full scale (4095) */
        int vel = 1 + (raw - 1000) * 126 / 3095;
        ESP_LOGD(TAG, "pad %d note %d peak %d vel %d", pad, note, raw, vel);
        midi_send(0x90 | channel, note, vel < 1 ? 1 : vel > 127 ? 127 : vel);
        maschine_set_pad_led(pad, 127, 127, 127);
    } else {
        midi_send(0x80 | channel, note, 0);
        pad_idle(pad);
    }
}

/* Public entry points: run the handler, then bring the button LEDs in line
 * with whatever changed (unchanged LEDs aren't resent) */
void editor_button(int bit, bool down)
{
    editor_button_impl(bit, down);
    update_button_leds();
}

void editor_encoder(int i, int delta)
{
    editor_encoder_impl(i, delta);
    update_button_leds();
}

void editor_main_encoder(int steps)
{
    editor_main_encoder_impl(steps);
    update_button_leds();
}

void editor_tick(int64_t now_us)
{
    editor_tick_impl(now_us);
    update_button_leds();
}

void editor_redraw(void)
{
    editor_redraw_impl();
    update_button_leds();
}

void editor_init(void)
{
    editor_init_impl();
    update_button_leds();
}

void editor_pad(int pad, bool down, int raw)
{
    editor_pad_impl(pad, down, raw);
    update_button_leds();
}

void editor_connected(void)
{
    editor_connected_impl();
    update_button_leds();
}
