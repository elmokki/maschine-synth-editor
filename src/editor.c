#include <dirent.h>
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
#include "minimax_params.h"
#include "storage.h"
#include "textentry.h"
#include "editor.h"

static const char *TAG = "editor";

#define PLUGIATOR_CHANNEL 0  /* MIDI channel 1; TODO make configurable */
#define PATCH_DIR STORAGE_ROOT "/plugiator/minimax"
#define MAX_PATCHES 200
#define MAX_PATCH_BYTES 512

#define COLUMN_WIDTH 64
#define COLUMN_INNER 63  /* x=255 is cropped, keep every column 63 px wide */
#define NONE (-1)

/* Encoder counter units per value step, per parameter type: ~1300 units
 * per turn, so continuous values move ~130 per turn and 6-position knobs
 * one position per ~1/16 turn. */
#define UNITS_CONT 10
#define UNITS_STEP6 80
#define UNITS_RAW24 50

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
#define BTN_BROWSE 10
#define BTN_ARROW_LEFT 12
#define BTN_ARROW_RIGHT 13
#define BTN_ARROW_LEFT_2 19
#define BTN_ARROW_RIGHT_2 20
#define BTN_ENTER 21
#define BTN_GROUP_A 24
#define BTN_REC 37
#define BTN_NAVIGATE 43

#define PAD_BASE_NOTE 36   /* pad 12 (bottom left) = C1, like drum pads */
#define FIRST_USER_PATCH 100  /* 000-099 are the factory presets */

/* The last loaded patch is restored at boot. Written once a patch has
 * stayed loaded this long, so browsing doesn't keep writing flash. */
#define STATE_DIR STORAGE_ROOT "/state"
#define LAST_PATCH_FILE STATE_DIR "/minimax_last"
#define LAST_PATCH_DELAY_US 5000000
#define DEFAULT_PATCH "099 Init"

/* Mono button LEDs: every button that does something right now is lit
 * dimly, brightly when it's on / active */
#define LED_DIM 8
#define LED_ON 127

/* Group button LEDs: dim = section exists (violet tint if it has several
 * pages); the selected section is cyan, and its page shows on the button's
 * left/right LEDs: page 1 = left, page 2 = right, page 3 = whole button.
 * A one-page section lights the whole button. */
#define LED_SECTION_DIM 0, 4, 12        /* one page */
#define LED_SECTION_DIM_MULTI 7, 0, 12  /* several pages: a slightly violet tint */
static const uint8_t led_section_on[3] = {30, 90, 127};
#define LED_HALF_DIM 6  /* percent of full for the unlit half */

/* Navigation is two-level, the same for every synth: Group buttons A-H
 * pick a section (e.g. an operator, tone or partial), the main encoder
 * steps through pages. Switching between sections keeps the page position within
 * the section, so parallel sections (OP1 EG -> OP3 EG) line up. */
typedef struct {
    uint8_t section;     /* 0-7 = Group A-H */
    const char *title;
    int8_t knobs[8];     /* parameter index per encoder, or NONE */
    int8_t switches[8];  /* parameter index per display button, or NONE */
} page_t;

/* Main panel of the plug-in GUI first, in signal-chain order (OSC -> MIX
 * -> FILTER -> LOUDNESS), then its global controllers, then the Add panel's
 * extras. Every switch sits above the knob it belongs with (the Decay
 * switch shapes the envelope release, so it's above A DECAY rather than in
 * the controllers section where the panel has it), and unrelated
 * parameters don't share a page just to fill columns. Bypass affects all
 * effects, so it's on both effect pages. */
#define _ NONE
static const page_t pages[] = {
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
#define PAGE_COUNT ((int)(sizeof(pages) / sizeof(pages[0])))

static uint8_t values[MINIMAX_PARAM_COUNT];
static bool known[MINIMAX_PARAM_COUNT];  /* the Plugiator can't tell us */
static int encoder_acc[8];
static int page;
static bool overview;  /* section list shown while Navigate is held */
static bool encoder_browses;  /* Browse toggled: main encoder picks patches, not pages */

static char *patch_names[MAX_PATCHES];
static int patch_count;
static int patch_loaded = NONE;
static int patch_browse = NONE;  /* selection while browsing, not loaded yet */
static int64_t browse_deadline;

static char last_saved[72];        /* contents of LAST_PATCH_FILE */
static int64_t remember_at;        /* when to write LAST_PATCH_FILE, 0 = nothing pending */
static bool restored;              /* last patch already restored since boot */

/* Store (Rec) and naming (pads as a phone keypad) screens */
typedef enum { MODE_EDIT, MODE_STORE, MODE_NAME } ui_mode_t;
static ui_mode_t mode;
static int store_target;  /* NONE = new patch, else index of a user patch */
static char store_name[TEXTENTRY_MAX_LEN + 1];
static int64_t delete_armed_until;  /* DELETE pressed once, press again to confirm */
#define DELETE_CONFIRM_US 3000000

static void load_patch_list(void);
static void draw_store(void);

/* ---- sections ---- */

static int section_first_page(int section)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pages[i].section == section) {
            return i;
        }
    }
    return NONE;
}

static int section_page_count(int section)
{
    int n = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        n += pages[i].section == section;
    }
    return n;
}

static void update_group_leds(void)
{
    for (int g = 0; g < 8; g++) {
        if (g == pages[page].section) {
            int count = section_page_count(g);
            int k = page - section_first_page(g);
            bool lit_l = count == 1 || k != 1;
            bool lit_r = count == 1 || k >= 1;
            int left = lit_l ? 100 : LED_HALF_DIM;
            int right = lit_r ? 100 : LED_HALF_DIM;
            uint8_t l[3], r[3];
            for (int i = 0; i < 3; i++) {
                l[i] = led_section_on[i] * left / 100;
                r[i] = led_section_on[i] * right / 100;
            }
            maschine_set_group_led_lr(g, l, r);
        } else if (section_page_count(g) > 1) {
            maschine_set_group_led(g, LED_SECTION_DIM_MULTI);
        } else if (section_first_page(g) != NONE) {
            maschine_set_group_led(g, LED_SECTION_DIM);
        } else {
            maschine_set_group_led(g, 0, 0, 0);
        }
    }
}

static void update_button_leds(void)
{
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
        return;
    }
    maschine_set_button_led(BTN_REC, LED_DIM);
    maschine_set_button_led(BTN_BROWSE, encoder_browses ? LED_ON : LED_DIM);
    for (int col = 0; col < 8; col++) {
        int sw = pages[page].switches[col];
        uint8_t v = 0;
        if (sw != NONE) {
            v = known[sw] && values[sw] >= 64 ? LED_ON : LED_DIM;
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

static void format_value(int p, char *buf, size_t len)
{
    const plug_param_t *prm = &minimax_params[p];
    uint8_t v = values[p];
    if (!known[p]) {
        snprintf(buf, len, "--");
    } else if (prm->type == PT_SWITCH) {
        snprintf(buf, len, v >= 64 ? "ON" : "OFF");
    } else if (prm->type == PT_STEP6) {
        int pos = (v + 11) / 22;
        pos = pos > 5 ? 5 : pos;
        if (prm->labels) {
            snprintf(buf, len, "%s", prm->labels[pos]);
        } else {
            snprintf(buf, len, "%d", pos + 1);
        }
    } else {
        snprintf(buf, len, "%d", v);
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

    int first = section_first_page(pages[page].section);
    int count = section_page_count(pages[page].section);
    if (count > 1) {
        snprintf(buf, sizeof(buf), "%c  %s  %d/%d", 'A' + pages[page].section,
                 pages[page].title, page - first + 1, count);
    } else {
        snprintf(buf, sizeof(buf), "%c  %s", 'A' + pages[page].section, pages[page].title);
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
        snprintf(buf, sizeof(buf), pending ? "%s  [ENTER]" : "%s", patch_names[shown]);
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

    if (encoder_browses) {
        draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "BROWSE: encoder picks patch", true);
    } else {
        /* Where the main encoder leads */
        snprintf(buf, sizeof(buf), "< %s", pages[(page + PAGE_COUNT - 1) % PAGE_COUNT].title);
        draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, buf, true);
        snprintf(buf, sizeof(buf), "%s >", pages[(page + 1) % PAGE_COUNT].title);
        draw_right(1, right, HEADER_CAP_Y, &font_small, buf, true);
    }
}

/* Two cycles of a Minimoog waveform (position 0-5 of the waveform knob)
 * in the box x, y, w, h */
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

static void draw_column(int col)
{
    if (overview || mode != MODE_EDIT) {
        return;
    }
    int disp = col / 4;
    int x = (col % 4) * COLUMN_WIDTH;
    const page_t *pg = &pages[page];
    display_fill_rect(disp, x, HEADER_LINE_Y + 1, COLUMN_WIDTH,
                      DISPLAY_HEIGHT - HEADER_LINE_Y - 1, false);

    /* The switch on this column's display button */
    int sw = pg->switches[col];
    if (sw != NONE) {
        bool on = known[sw] && values[sw] >= 64;
        display_round_rect(disp, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, on, true);
        draw_centered(disp, x, COLUMN_INNER,
                      SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, minimax_params[sw].short_name, !on);
    }

    /* The encoder's parameter: name, value, bar */
    int p = pg->knobs[col];
    if (p == NONE) {
        return;
    }
    const plug_param_t *prm = &minimax_params[p];
    draw_centered(disp, x, COLUMN_INNER, NAME_CAP_Y, &font_small, prm->short_name, true);

    char val[16];
    format_value(p, val, sizeof(val));
    if (prm->icon == PI_WAVE && known[p]) {
        /* Waveform picture in the value area, its name below instead of
         * the bar */
        int pos = (values[p] + 11) / 22;
        draw_wave(disp, x + 9, VALUE_CAP_Y - 1, COLUMN_INNER - 18, font_large.cap_height,
                  pos > 5 ? 5 : pos);
        draw_centered(disp, x, COLUMN_INNER, BAR_Y, &font_tiny, val, true);
        return;
    }
    /* Largest font the value fits in, centred on the large font's caps */
    const font_t *vf = &font_large;
    if (display_text_width(vf, val) > COLUMN_INNER - 2) {
        vf = display_text_width(&font_medium, val) <= COLUMN_INNER - 2 ? &font_medium : &font_small;
    }
    draw_centered(disp, x, COLUMN_INNER,
                  VALUE_CAP_Y + (font_large.cap_height - vf->cap_height) / 2, vf, val, true);

    if (known[p] && prm->type != PT_SWITCH) {
        int max = prm->type == PT_RAW24 ? 24 : prm->type == PT_STEP6 ? 110 : 127;
        display_rect(disp, x + 4, BAR_Y, COLUMN_INNER - 8, BAR_H, true);
        display_fill_rect(disp, x + 6, BAR_Y + 2, (COLUMN_INNER - 12) * values[p] / max, BAR_H - 4, true);
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
        bool sel = g == pages[page].section;
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
        const font_t *nf = display_text_width(&font_small, pages[first].title) <= COLUMN_INNER - 4
                           ? &font_small : &font_tiny;
        char name[24];
        fit_text(nf, pages[first].title, COLUMN_INNER - 4, name, sizeof(name));
        draw_centered(d, x, COLUMN_INNER, 40, nf, name, !sel);
        int count = section_page_count(g);
        if (count > 1) {
            snprintf(buf, sizeof(buf), "%d pages", count);
            draw_centered(d, x, COLUMN_INNER, 52, &font_small, buf, !sel);
        }
    }
}

static void draw_page(void)
{
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

/* ---- MIDI ---- */

static void send_param(int p)
{
    const plug_param_t *prm = &minimax_params[p];
    uint8_t status = (prm->msg == PLUG_CC ? 0xB0 : 0xA0) | PLUGIATOR_CHANNEL;
    midi_send(status, prm->number, values[p]);
}

static int find_param(uint8_t status, uint8_t number)
{
    plug_msg_t msg = (status & 0xF0) == 0xB0 ? PLUG_CC : PLUG_PAT;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        if (minimax_params[p].msg == msg && minimax_params[p].number == number) {
            return p;
        }
    }
    return NONE;
}

static void load_patch(int idx)
{
    char path[sizeof(PATCH_DIR) + 72];
    snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, patch_names[idx]);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "can't open %s", path);
        return;
    }
    uint8_t buf[MAX_PATCH_BYTES];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    /* Patch files are plain 3-byte CC / poly aftertouch messages with the
     * channel nibble 0 (see tools/plugiator_import.py) */
    for (size_t i = 0; i + 3 <= n; i += 3) {
        uint8_t type = buf[i] & 0xF0;
        if (type != 0xB0 && type != 0xA0) {
            continue;
        }
        midi_send(type | PLUGIATOR_CHANNEL, buf[i + 1], buf[i + 2]);
        int p = find_param(type, buf[i + 1]);
        if (p != NONE) {
            values[p] = buf[i + 2];
            known[p] = true;
        }
    }
    patch_loaded = idx;
    remember_at = esp_timer_get_time() + LAST_PATCH_DELAY_US;
    ESP_LOGI(TAG, "loaded %s (%u bytes)", patch_names[idx], (unsigned)n);
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
    int n = FIRST_USER_PATCH;
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

static int unknown_param_count(void)
{
    int n = 0;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        n += minimax_params[p].in_preset && !known[p];
    }
    return n;
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
    int unknown = unknown_param_count();
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
    set_store_target(patch_loaded != NONE && patch_number(patch_loaded) >= FIRST_USER_PATCH
                     ? patch_loaded : NONE);
    draw_page();
}

static void leave_store(void)
{
    textentry_stop();
    mode = MODE_EDIT;
    for (int pad = 0; pad < 16; pad++) {
        maschine_set_pad_led(pad, 0, 0, 20);
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    draw_page();
}

static void store_main_encoder(int steps)
{
    /* Targets: NONE (new), then the user patches in list order */
    int targets[MAX_PATCHES + 1], n = 0, cur = 0;
    targets[n++] = NONE;
    for (int i = 0; i < patch_count; i++) {
        if (patch_number(i) >= FIRST_USER_PATCH) {
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
    char file[TEXTENTRY_MAX_LEN + 8], path[sizeof(PATCH_DIR) + sizeof(file) + 1];
    snprintf(file, sizeof(file), "%03d %s", number, name);
    snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, file);

    /* Same format as the imported presets: every in-preset parameter as a
     * 3-byte message, channel nibble 0, in table order */
    uint8_t buf[MINIMAX_PARAM_COUNT * 3];
    size_t n = 0;
    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        const plug_param_t *prm = &minimax_params[p];
        if (!prm->in_preset) {
            continue;
        }
        buf[n++] = prm->msg == PLUG_CC ? 0xB0 : 0xA0;
        buf[n++] = prm->number;
        buf[n++] = known[p] ? values[p] : prm->default_value;
    }
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, n, f) != n) {
        ESP_LOGE(TAG, "writing %s failed", path);
        if (f) {
            fclose(f);
        }
        return;
    }
    fclose(f);
    if (store_target != NONE && strcmp(patch_names[store_target], file) != 0) {
        char old[sizeof(path)];
        snprintf(old, sizeof(old), "%s/%s", PATCH_DIR, patch_names[store_target]);
        remove(old);  /* renamed */
    }
    ESP_LOGI(TAG, "stored %s (%u bytes)", file, (unsigned)n);

    load_patch_list();
    patch_loaded = NONE;
    for (int i = 0; i < patch_count; i++) {
        if (strcmp(patch_names[i], file) == 0) {
            patch_loaded = i;
        }
    }
    remember_at = esp_timer_get_time() + LAST_PATCH_DELAY_US;
    leave_store();
}

static void delete_patch(void)
{
    char path[sizeof(PATCH_DIR) + 72];
    snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, patch_names[store_target]);
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "deleting %s failed", path);
        return;
    }
    ESP_LOGI(TAG, "deleted %s", patch_names[store_target]);
    bool was_loaded = store_target == patch_loaded;
    char loaded_name[72] = "";
    if (patch_loaded != NONE && !was_loaded) {
        snprintf(loaded_name, sizeof(loaded_name), "%s", patch_names[patch_loaded]);
    }
    load_patch_list();
    /* The loaded patch's index may have shifted; the sound itself is still
     * what the Plugiator plays, so only forget the name if it was this one */
    patch_loaded = NONE;
    for (int i = 0; i < patch_count && loaded_name[0]; i++) {
        if (strcmp(patch_names[i], loaded_name) == 0) {
            patch_loaded = i;
        }
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
                maschine_set_pad_led(pad, 0, 0, 20);
            }
            draw_store();
        }
    }
}

/* ---- input ---- */

static void editor_encoder_impl(int i, int delta)
{
    if (mode != MODE_EDIT) {
        return;
    }
    end_browse();
    int p = pages[page].knobs[i];
    if (p == NONE) {
        return;
    }
    const plug_param_t *prm = &minimax_params[p];
    int units = prm->type == PT_STEP6 ? UNITS_STEP6 :
                prm->type == PT_RAW24 ? UNITS_RAW24 : UNITS_CONT;
    encoder_acc[i] += delta;
    int steps = encoder_acc[i] / units;
    if (steps == 0) {
        return;
    }
    encoder_acc[i] -= steps * units;

    /* Unknown value: start from the factory default instead of guessing */
    int v = known[p] ? values[p] : prm->default_value;
    if (prm->type == PT_STEP6) {
        int pos = (v + 11) / 22 + steps;
        v = (pos < 0 ? 0 : pos > 5 ? 5 : pos) * 22;
    } else if (prm->type == PT_SWITCH) {
        v = steps > 0 ? 127 : 0;
    } else {
        int max = prm->type == PT_RAW24 ? 24 : 127;
        v += steps;
        v = v < 0 ? 0 : v > max ? max : v;
    }
    if (known[p] && v == values[p]) {
        return;
    }
    values[p] = v;
    known[p] = true;
    send_param(p);
    draw_column(i);
}

static void go_to_page(int new_page)
{
    page = new_page;
    memset(encoder_acc, 0, sizeof(encoder_acc));
    draw_page();
    update_group_leds();
}

static void change_page(int d)
{
    go_to_page((page + d + PAGE_COUNT) % PAGE_COUNT);
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
    if (section == pages[page].section) {
        /* Already here: step to the section's next page */
        go_to_page(first + (page - first + 1) % count);
        return;
    }
    /* Keep the position within the section, clamped to its length */
    int offset = page - section_first_page(pages[page].section);
    go_to_page(first + (offset < count ? offset : count - 1));
}

static void editor_button_impl(int bit, bool down)
{
    if (mode != MODE_EDIT) {
        if (down) {
            store_button(bit);
        }
        return;
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
        int sw = pages[page].switches[col];
        if (sw != NONE) {
            /* An unknown switch is drawn as off, so its first press turns
             * it on - toggling from the factory default could look like
             * nothing happened */
            bool on = known[sw] ? values[sw] < 64 : true;
            values[sw] = on ? 127 : 0;
            known[sw] = true;
            send_param(sw);
            draw_column(col);
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
    } else if (bit == BTN_REC) {
        enter_store();
    } else if (bit == BTN_ENTER && patch_browse != NONE) {
        load_patch(patch_browse);
        patch_browse = NONE;
        draw_page();
    }
}

static void editor_main_encoder_impl(int steps)
{
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
    mkdir(STATE_DIR, 0777);  /* fails harmlessly if it exists */
    FILE *f = fopen(LAST_PATCH_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "can't write %s", LAST_PATCH_FILE);
        return;
    }
    fputs(patch_names[patch_loaded], f);
    fclose(f);
    snprintf(last_saved, sizeof(last_saved), "%s", patch_names[patch_loaded]);
    ESP_LOGI(TAG, "remembered %s for next boot", last_saved);
}

static void editor_tick_impl(int64_t now_us)
{
    textentry_tick(now_us);
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
    DIR *dir = opendir(PATCH_DIR);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) && patch_count < MAX_PATCHES) {
            patch_names[patch_count++] = strdup(e->d_name);
        }
        closedir(dir);
        qsort(patch_names, patch_count, sizeof(char *), cmp_names);
    }
    ESP_LOGI(TAG, "%d MiniMax patches in %s", patch_count, PATCH_DIR);
}

static void editor_init_impl(void)
{
    load_patch_list();
    FILE *f = fopen(LAST_PATCH_FILE, "r");
    if (f) {
        if (!fgets(last_saved, sizeof(last_saved), f)) {
            last_saved[0] = '\0';
        }
        fclose(f);
    }

    for (int p = 0; p < MINIMAX_PARAM_COUNT; p++) {
        values[p] = minimax_params[p].default_value;
    }
    for (int pad = 0; pad < 16; pad++) {
        maschine_set_pad_led(pad, 0, 0, 20);
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    draw_page();
    update_group_leds();
}

/* First connection since boot: bring the Plugiator back to the patch that
 * was loaded last (or the init patch), so the editor's values are known */
static void editor_connected_impl(void)
{
    if (restored) {
        return;
    }
    restored = true;
    const char *want = last_saved[0] ? last_saved : DEFAULT_PATCH;
    for (int i = 0; i < patch_count; i++) {
        if (strcmp(patch_names[i], want) == 0) {
            load_patch(i);
            remember_at = 0;  /* it's already the remembered one */
            draw_page();
            return;
        }
    }
    ESP_LOGW(TAG, "patch to restore not found: %s", want);
}

/* Pads play notes on the Plugiator's channel, or type in the name screen */
static void editor_pad_impl(int pad, bool down, int raw)
{
    if (mode == MODE_NAME) {
        if (down) {
            textentry_pad(pad, esp_timer_get_time());
        }
        return;
    }
    /* Pad 12 (bottom left) is the lowest note, rows go up like a keyboard */
    int note = PAD_BASE_NOTE + (3 - pad / 4) * 4 + pad % 4;
    if (down) {
        /* Crude velocity from the pressure when the press is detected */
        int vel = raw / 32;
        midi_send(0x90 | PLUGIATOR_CHANNEL, note, vel < 1 ? 1 : vel > 127 ? 127 : vel);
        maschine_set_pad_led(pad, 127, 127, 127);
    } else {
        midi_send(0x80 | PLUGIATOR_CHANNEL, note, 0);
        maschine_set_pad_led(pad, 0, 0, 20);
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
