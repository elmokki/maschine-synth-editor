#include <ctype.h>
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
#define UNITS_SETTING 80  /* settings step a little slower than knobs */

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
#include "buttons.h"
#define VOLUME_STEP 4    /* per main encoder detent */

/* The last loaded patch is restored at boot, and settings persist. Both
 * are written once things have stayed unchanged this long, so browsing or
 * turning an encoder doesn't keep writing flash. */
#define STATE_DIR STORAGE_ROOT "/state"
#define SETTINGS_DIR STORAGE_ROOT "/settings"
#define GLOBAL_SETTINGS_FILE SETTINGS_DIR "/global"

/* The pads area of the device is never per synth: the pads' lowest note,
 * scale and key are global (Pad Mode screen). Pad notes go chromatically
 * up from the lowest note, or up the scale from the first scale note at or
 * above it. */
typedef struct {
    const char *name;
    uint8_t count;
    uint8_t steps[12];
} scale_t;
static const scale_t scales[] = {
    {"CHROMATIC", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"HARM MIN", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"MIXOLYD", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"PENTA MAJ", 5, {0, 2, 4, 7, 9}},
    {"PENTA MIN", 5, {0, 3, 5, 7, 10}},
    {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
};
#define SCALE_COUNT ((int)(sizeof(scales) / sizeof(scales[0])))
/* Pad Mode screen settings (ids beyond any synth setting index) */
#define PAD_LOW_ID 100
#define PAD_SCALE_ID 101
#define PAD_KEY_ID 102
#define PAD_CHORD_ID 105
#define PAD_LOW_DEFAULT 48  /* C3: lowest pad, bottom left */
/* Global settings, on the Setup screen's right display (the synth's own
 * are on the left, so at most 4) */
#define SETUP_SYNTH_COLS 4
#define MIDI_THRU_ID 103
#define LED_BRIGHT_ID 104
#define NOTES_IN_ID 106
#define HANG_NOTES_ID 107
/* Notes arriving on DIN IN on the synth's channel act like pad hits: the
 * pad lights, and e.g. the D-110 rhythm picks that key */
static int notes_in;
/* LED brightness 10-100 % in 10 % steps, as a gain on the colours as
 * defined (see maschine_set_led_gain): 10 % = x0.5, 50 % (the default) =
 * x1.25 (dim ones a bit brighter, full ones as they are), 100 % = x2.5 for
 * daylight. Full colours only go a little paler, and only above 50 %. */
#define LED_BRIGHT_MIN 1
#define LED_BRIGHT_MAX 10
#define LED_BRIGHT_DEFAULT 5
static int led_bright = LED_BRIGHT_DEFAULT;  /* tens of percent */
#define LED_GAIN_PCT(b) ((b) <= 5 ? 50 + ((b) - 1) * 75 / 4 : 125 + ((b) - 5) * 25)
#define LED_WHITE_PCT(b) ((b) <= 5 ? 0 : ((b) - 5) * 4)  /* 20 % at 100 % */
static int midi_thru = MIDI_THRU_OFF;
static const char *const thru_names[] = {"OFF", "THRU", "TO SYNTH"};
static int pad_low = PAD_LOW_DEFAULT;
static int pad_scale, pad_key;  /* index into scales, 0-11 = C-B */
/* The notes each pad started (a chord in chord mode), so a release stops
 * exactly those even if the settings changed meanwhile */
static uint8_t pad_notes[16][4], pad_note_count[16];
/* ...and the channel they started on: a release goes there, so changing the
 * part (the pads' channel) with a pad held doesn't leave the note ringing -
 * unless HANG NOTES is on (Setup), which sends it to the channel now and
 * leaves the old one's note on: good for playing with */
static uint8_t pad_channel[16];
static int hang_notes;
/* Chord mode: a pad plays the triad built on its note (thirds up the
 * scale), with Shift held the seventh chord */
static int pad_chord;
#define WRITE_DELAY_US 5000000
#define MAX_SETTINGS 8

/* Mono button LEDs: every button that does something right now is lit
 * dimly, brightly when it's on / active */
#define LED_DIM 9  /* 11 at the default LED brightness (x1.25) */
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

/* The synths to pick from (Step), and the active one */
static const synth_t *const synths[] = {&synth_minimax, &synth_b4000, &synth_volca, &synth_d110, &synth_d110p, &synth_d110r, &synth_jv880, &synth_jv880r, &synth_jv880p, &synth_u110, &synth_wsp};
#define SYNTH_COUNT ((int)(sizeof(synths) / sizeof(synths[0])))
#define ACTIVE_SYNTH_FILE STORAGE_ROOT "/state/active_synth"
#define SYNTH_CONFIRM_US 3000000
static const synth_t *synth = &synth_minimax;

/* A long job in the driver (see synth_t.busy) */
static bool synth_busy(void) { return synth->busy && synth->busy(); }
static bool busy_shown;
static int64_t busy_draw_at;
static bool usb_ready;              /* Maschine (and DIN MIDI) connected */
static int64_t activating_until;    /* synth being made ready, patch follows */
static int synth_confirm = -1;      /* synth pressed once with unsaved edits */
/* The synth list: one card stack per device (consecutive synths with the
 * same `device`), one display column each; the encoder under it browses its
 * stack, the button opens the card on top. Devices past the 8th don't show. */
#define MAX_DEVICES 8
static int device_count, device_first[MAX_DEVICES], device_size[MAX_DEVICES];
static int stack_top[MAX_DEVICES];  /* synth index of each stack's selected card */
static int stack_y[MAX_DEVICES];    /* where that card's top edge is drawn now: slides to its slot */
static int64_t stack_anim_at;
static bool volume_held;
static int64_t synth_confirm_until;

static int encoder_acc[8];
static int page;
static uint8_t section_last[8];  /* page each section was last left on */
static bool shift_held;
static int tip_page;  /* tutorial (Shift + Control): the page shown */
/* Per instance (synth_t.instance, e.g. D-110 part): its library patch */
#define MAX_INSTANCES 16
static int inst_loaded[MAX_INSTANCES];
static bool inst_modified[MAX_INSTANCES];
static bool inst_init;
static int64_t action_armed_until;  /* Setup's action button pressed once */
#define ACTION_CONFIRM_US 3000000
static bool overview;  /* section list shown while Navigate is held */
static bool encoder_browses;  /* Browse toggled: main encoder picks patches, not pages */

static char *patch_names[MAX_PATCHES];
/* Categories, see synth_t.categories: by patch number; the browse filter
 * is a mask of categories 1-8 (bit 0 = 1); all on (the default) = every
 * patch, also those without a category */
#define ALL_CATEGORIES 0xFF
#define CATEGORY_UNITS 60  /* encoder counter units per category step */
#define CATEGORY_FILE "categories"
#define MAX_PATCH_NUMBER 1000
static uint8_t category_by_number[MAX_PATCH_NUMBER];
static uint8_t category_filter = ALL_CATEGORIES;
static uint8_t store_category;  /* given to the patch being stored */
static int unit_slot;                /* synth_t.unit_store: slot in the instrument */
static int64_t unit_store_armed_until;
#define UNIT_SLOT_UNITS 40
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
/* A short confirmation in the right display's header, e.g. after storing */
#define NOTICE_US 2000000
static char notice[48];
static int64_t notice_until;

static int16_t setting_values[MAX_SETTINGS];
static int64_t settings_save_at;   /* when to write the settings file, 0 = nothing pending */

/* Store (Rec), naming (pads as a phone keypad) and setup (Control) screens */
typedef enum { MODE_EDIT, MODE_STORE, MODE_NAME, MODE_SETUP, MODE_SYNTH, MODE_CATEGORY, MODE_PADS, MODE_TUTORIAL,
               MODE_OVERVIEW } ui_mode_t;
static ui_mode_t mode;

/* Screens opened by their own button (Step, All, Pad Mode, Control) */
static bool is_menu(ui_mode_t m)
{
    return m == MODE_SYNTH || m == MODE_CATEGORY || m == MODE_PADS || m == MODE_SETUP ||
           m == MODE_TUTORIAL || m == MODE_OVERVIEW;
}

static int store_target;  /* NONE = new patch, else index of a user patch */
static char store_name[TEXTENTRY_MAX_LEN + 1];
static int64_t delete_armed_until;  /* DELETE pressed once, press again to confirm */
#define DELETE_CONFIRM_US 3000000

static void load_patch_list(void);
static void draw_store(void);
static void draw_setup(void);
static void draw_synths(void);
static void draw_categories(void);
static void draw_page(void);
static void draw_patch_overview(void);
static int po_page;  /* the patch overview's page (Grid) */
static void end_browse(void);
static void draw_pads(void);
static void draw_tutorial(void);

int synth_setting(int index)
{
    return index < synth->setting_count ? setting_values[index] : 0;
}

/* While Shift is held, the buttons with a Shift function pulse: a factor
 * 40-100 %, stepped every 50 ms (an LED changing on every editor tick would
 * keep the LED reports so busy that the displays never get sent) */
#define SHIFT_PULSE_PERIOD_US 900000
#define SHIFT_PULSE_STEP_US 50000
static int shift_pulse_pct(void)
{
    int64_t t = esp_timer_get_time() / SHIFT_PULSE_STEP_US * SHIFT_PULSE_STEP_US;
    float ph = (float)(t % SHIFT_PULSE_PERIOD_US) / SHIFT_PULSE_PERIOD_US;
    return 70 + (int)(30.0f * cosf(2.0f * (float)M_PI * ph));
}

static uint8_t pulse(uint8_t v)
{
    return shift_held ? (uint8_t)(v * shift_pulse_pct() / 100) : v;
}

/* Device volume (Volume button + main encoder): a CC kept in a setting, or
 * the driver's own */
static bool has_volume(void) { return synth->volume_cc || synth->volume_set; }

static int volume_value(void)
{
    return synth->volume_get ? synth->volume_get() : synth_setting(synth->volume_setting);
}

/* The channel the pads, MIDI thru and panic use */
static int play_channel(void)
{
    return synth->play_channel ? synth->play_channel() : synth_setting(SETTING_CHANNEL);
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

/* Pad 12 (bottom left) is the lowest note, rows go up like a keyboard;
 * chromatic from PAD LOW, or up the scale from its key */
/* Note k steps up the pads' scale from the lowest pad */
static bool in_scale(int note)
{
    const scale_t *sc = &scales[pad_scale];
    int deg = ((note - pad_key) % 12 + 12) % 12;
    for (int i = 0; i < sc->count; i++) {
        if (sc->steps[i] == deg) {
            return true;
        }
    }
    return false;
}

/* The note `steps` scale notes from `note` (chromatic: semitones); -1 if
 * past 0-127 */
static int scale_step(int note, int steps)
{
    int dir = steps < 0 ? -1 : 1;
    for (int left = steps * dir; left > 0;) {
        note += dir;
        if (note < 0 || note > 127) {
            return -1;
        }
        left -= pad_scale == 0 || in_scale(note);
    }
    return note;
}

/* The lowest pad: PAD LOW, or the first scale note from it up */
static int lowest_pad_note(void)
{
    int note = pad_low;
    while (pad_scale != 0 && note < 127 && !in_scale(note)) {
        note++;
    }
    return note;
}

/* Note k steps up from the lowest pad, along the scale */
static int scale_note(int k)
{
    int note = scale_step(lowest_pad_note(), k);
    return note < 0 ? 127 : note;
}

static int pad_index(int pad) { return (3 - pad / 4) * 4 + pad % 4; }

static int pad_note(int pad) { return scale_note(pad_index(pad)); }

int synth_scale_note(int note, int steps) { return scale_step(note, steps); }

/* The pad's chord: thirds up the scale (chromatic: major, dominant 7th) */
static int pad_chord_notes(int pad, bool seventh, uint8_t out[4])
{
    int k = pad_index(pad), n = seventh ? 4 : 3;
    for (int i = 0; i < n; i++) {
        int note = pad_scale == 0 ? scale_note(k) + (int[]){0, 4, 7, 10}[i] : scale_note(k + 2 * i);
        out[i] = note > 127 ? 127 : note;
    }
    return n;
}

/* "Dm3", "G7 2"-style name of a pad's chord, from its intervals */
static void chord_name(int pad, char *buf, size_t len)
{
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint8_t n[4];
    pad_chord_notes(pad, false, n);
    int third = n[1] - n[0], fifth = n[2] - n[0];
    const char *q = third == 4 && fifth == 7 ? "" : third == 3 && fifth == 7 ? "m"
                    : third == 3 && fifth == 6 ? "dim" : third == 4 && fifth == 8 ? "aug"
                    : third == 2 && fifth == 7 ? "sus2" : third == 5 && fifth == 7 ? "sus4" : "*";
    snprintf(buf, len, "%s%s%d", names[n[0] % 12], q, n[0] / 12 - 1);
}

/* Pads at rest, in the synth's theme; the key's notes brighter, so the
 * octaves show (at high LED brightness they go paler instead, see
 * maschine_set_led_gain) */
#define PAD_ROOT_GAIN 3
static void pad_idle(int pad)
{
    const uint8_t *c = theme_dim();
    int g = pad_note(pad) % 12 == pad_key ? PAD_ROOT_GAIN : 1;
    maschine_set_pad_led(pad, c[0] * g, c[1] * g, c[2] * g);
}

static void update_group_leds(void)
{
    if (synth_busy()) {
        for (int g = 0; g < 8; g++) {
            maschine_set_group_led(g, 0, 0, 0);
        }
        return;
    }
    if (mode != MODE_EDIT && !is_menu(mode)) {
        for (int g = 0; g < 8; g++) {
            maschine_set_group_led(g, 0, 0, 0);  /* sections can't be picked here */
        }
        return;
    }
    if (mode == MODE_TUTORIAL) {
        /* The page's Group buttons lit, the rest dark */
        const synth_tip_t *tip = &synth->tips[tip_page];
        for (int g = 0; g < 8; g++) {
            bool lit = false;
            for (int k = 0; k < 8; k++) {
                lit |= tip->buttons[k] == BTN_GROUP_A + g;
            }
            const uint8_t *c = theme_bright();
            maschine_set_group_led(g, lit ? c[0] : 0, lit ? c[1] : 0, lit ? c[2] : 0);
        }
        return;
    }
    if (shift_held && synth->shift_group_led) {
        for (int g = 0; g < 8; g++) {
            int v = synth->shift_group_led(g);
            const uint8_t *c = v > 0 ? theme_bright() : theme_dim();
            if (v < 0) {
                maschine_set_group_led(g, 0, 0, 0);
            } else {
                maschine_set_group_led(g, pulse(c[0]), pulse(c[1]), pulse(c[2]));
            }
        }
        return;
    }
    for (int g = 0; g < 8; g++) {
        uint8_t own[3];
        bool own_dim = synth->section_dim && synth->section_dim(g, own);
        if (g == synth->pages[page].section && mode == MODE_EDIT) {  /* not while a menu covers it */
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

/* Every button that does something on the current screen is lit (dim, or
 * bright when active); everything else is dark */
static void update_button_leds(void)
{
    if (synth_busy()) {
        /* each LED set once to its final value (see the tutorial's note) */
        for (int bit = 0; bit < 48; bit++) {
            if (bit < BTN_GROUP_A || bit >= BTN_GROUP_A + 8) {
                maschine_set_button_led(bit, bit == BTN_ERASE && synth->busy_cancel ? LED_ON : 0);
            }
        }
        return;
    }
    if (mode == MODE_TUTORIAL) {
        /* Only the page's buttons lit; the arrows (pages) and Control (out)
         * dim; everything else dark */
        /* Each LED set once to its final value: this runs on every tick,
         * and an LED changed twice per call kept the LED reports so busy
         * that the displays never got sent */
        const synth_tip_t *tip = &synth->tips[tip_page];
        for (int bit = 0; bit < 48; bit++) {
            if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8) {
                continue;  /* RGB: update_group_leds */
            }
            uint8_t v = bit == BTN_ARROW_LEFT || bit == BTN_ARROW_RIGHT || bit == BTN_ARROW_LEFT_2 ||
                                bit == BTN_ARROW_RIGHT_2 || bit == BTN_CONTROL
                            ? LED_DIM
                            : 0;
            for (int k = 0; k < 8; k++) {
                if (tip->buttons[k] == bit) {
                    v = LED_ON;
                }
            }
            maschine_set_button_led(bit, v);
        }
        return;
    }
    bool edit = mode == MODE_EDIT, store = mode == MODE_STORE || mode == MODE_NAME;
    /* Panic works on every screen */
    maschine_set_button_led(BTN_MUTE, panic_flash_until ? LED_ON : LED_DIM);
    /* Menus open from the editor and from each other */
    bool menus = edit || is_menu(mode);
    /* Shift + Control: the tutorial */
    maschine_set_button_led(BTN_CONTROL, shift_held && synth->tips && menus ? pulse(LED_ON)
                                         : mode == MODE_SETUP ? LED_ON : menus ? LED_DIM : 0);
    maschine_set_button_led(BTN_STEP, mode == MODE_SYNTH ? LED_ON : menus ? LED_DIM : 0);
    maschine_set_button_led(BTN_PAD_MODE, mode == MODE_PADS ? LED_ON : menus ? LED_DIM : 0);
    maschine_set_button_led(BTN_GRID, !synth->overview_draw ? 0 : mode == MODE_OVERVIEW ? LED_ON : menus ? LED_DIM : 0);
    maschine_set_button_led(BTN_ALL, !synth->categories ? 0
                                     : mode == MODE_CATEGORY || (menus && category_filter != ALL_CATEGORIES)
                                     ? LED_ON
                                     : menus ? LED_DIM : 0);
    maschine_set_button_led(BTN_REC, store ? LED_ON : edit ? LED_DIM : 0);
    maschine_set_button_led(BTN_ENTER, store ? LED_DIM
                                       : !edit ? 0
                                       : patch_browse != NONE && patch_browse != patch_loaded ? LED_ON
                                       : LED_DIM);
    maschine_set_button_led(BTN_VOLUME, !edit || !has_volume() ? 0 : volume_held ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_BROWSE, !edit ? 0 : encoder_browses ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_ERASE, mode == MODE_CATEGORY ? LED_DIM
                                       : !edit ? 0 : init_armed_until || erase_held ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_NAVIGATE, !edit ? 0 : overview ? LED_ON : LED_DIM);
    /* Shift does something everywhere the menus are (Shift + Control: the
     * tutorial), and more on some synths and screens */
    bool overview_on = mode == MODE_OVERVIEW;
    bool shift_useful = (menus && synth->tips) || mode == MODE_CATEGORY || ((edit || overview_on) && synth->shift_group);
    maschine_set_button_led(BTN_SHIFT, !shift_useful ? 0 : shift_held ? LED_ON : LED_DIM);
    maschine_set_button_led(BTN_AUTO, edit && synth->read ? LED_DIM : 0);
    uint8_t step_led = !(edit || overview_on) || !synth->instance_step ? 0
                       : shift_held && synth->shift_step ? pulse(LED_ON) : LED_DIM;
    maschine_set_button_led(BTN_STEP_LEFT, step_led);
    maschine_set_button_led(BTN_STEP_RIGHT, step_led);
    static const int arrows[] = {BTN_ARROW_LEFT, BTN_ARROW_RIGHT, BTN_ARROW_LEFT_2, BTN_ARROW_RIGHT_2};
    for (size_t i = 0; i < sizeof(arrows) / sizeof(arrows[0]); i++) {
        maschine_set_button_led(arrows[i], edit || (overview_on && synth->overview_pages > 1) ? LED_DIM : 0);
    }

    /* Display buttons: what the screen offers */
    bool store_keys[8] = {true, true, store_target != NONE, true, synth->categories != NULL,
                          false, false, synth->unit_store != NULL};
    static const bool name_keys[8] = {true, false, false, true};
    for (int col = 0; col < 8; col++) {
        uint8_t v = 0;
        if (edit) {
            int sw = synth->pages[page].switches[col];
            if (sw != NONE) {
                v = synth->param_on(sw) ? LED_ON : LED_DIM;
            }
        } else if (mode == MODE_SYNTH) {
            if (col < device_count) {
                int i = stack_top[col];
                v = synths[i] == synth || i == synth_confirm ? LED_ON : LED_DIM;
            }
        } else if (mode == MODE_CATEGORY) {
            v = category_filter & (1 << col) ? LED_ON : LED_DIM;
            v = shift_held ? pulse(v) : v;  /* Shift + category: solo */
        } else if (mode == MODE_SETUP) {
            v = col == 7 && synth->action ? (action_armed_until ? LED_ON : LED_DIM) : 0;
        } else if (mode == MODE_STORE) {
            v = store_keys[col] ? LED_DIM : 0;
        } else if (mode == MODE_NAME) {
            v = name_keys[col] ? LED_DIM : 0;
        }
        maschine_set_button_led(BTN_DISPLAY_1 + col, v);
    }
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
    if (overview || mode != MODE_EDIT || synth_busy()) {
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
    if (synth->header_tag) {
        size_t n = strlen(buf);
        if (n + 3 < sizeof(buf)) {
            buf[n++] = ' ';
            buf[n++] = ' ';
            synth->header_tag(buf + n, sizeof(buf) - n);
        }
    }
    char title[64];
    snprintf(title, sizeof(title), "%s", buf);

    int shown = patch_browse != NONE ? patch_browse : patch_loaded;
    /* While browsing, a patch other than the loaded one is shown inverted
     * with an Enter hint; landing back on the loaded one shows it plain */
    bool pending = patch_browse != NONE && patch_browse != patch_loaded;
    char name[64];
    char own[24];
    if (shown == NONE && synth->sound_name && synth->sound_name(own, sizeof(own))) {
        snprintf(buf, sizeof(buf), modified ? "%s *" : "%s", own);  /* not from the library */
    } else if (shown == NONE) {
        snprintf(buf, sizeof(buf), "no patch loaded");
    } else {
        snprintf(buf, sizeof(buf), pending ? "%s  [ENTER]" : modified ? "%s *" : "%s",
                 patch_names[shown]);
    }
    int right = DISPLAY_USABLE_WIDTH - 2;
    /* Browsing patches, the name comes first: the page title keeps what room
     * is left of the row */
    int title_room = patch_browse != NONE ? right - display_text_width(&font_small, buf) - 10 : right;
    int title_end = 1;
    if (patch_browse == NONE || title_room >= 3 * font_small.height / 2) {
        char fitted[64];
        fit_text(&font_small, title, title_room, fitted, sizeof(fitted));
        title_end = draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, fitted, true);
    }
    fit_text(&font_small, buf, right - title_end - 10, name, sizeof(name));
    if (pending) {
        int w = display_text_width(&font_small, name);
        display_fill_rect(0, right - w - 2, 0, w + 4, HEADER_LINE_Y, true);
        draw_right(0, right, HEADER_CAP_Y, &font_small, name, false);
    } else {
        draw_right(0, right, HEADER_CAP_Y, &font_small, name, true);
    }

    if (volume_held || notice_until) {
        if (volume_held) {
            snprintf(buf, sizeof(buf), "VOLUME %d", volume_value());
        } else {
            fit_text(&font_small, notice, DISPLAY_USABLE_WIDTH - 4, buf, sizeof(buf));
        }
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
    if (overview || mode != MODE_EDIT || synth_busy()) {
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

void synth_redraw(void)
{
    if (mode == MODE_OVERVIEW && !activating_until) {
        draw_patch_overview();  /* e.g. another wave, or a sound read from the unit */
        return;
    }
    if (mode == MODE_EDIT && !overview && !activating_until) {
        draw_page();
        update_group_leds();
    }
}

void synth_patches_changed(void)
{
    /* Find the loaded patch again: its index may have moved */
    char loaded_name[72] = "";
    if (patch_loaded != NONE) {
        snprintf(loaded_name, sizeof(loaded_name), "%s", patch_names[patch_loaded]);
    }
    end_browse();
    load_patch_list();
    patch_loaded = NONE;
    for (int i = 0; i < patch_count && loaded_name[0]; i++) {
        if (strcmp(patch_names[i], loaded_name) == 0) {
            patch_loaded = i;
        }
    }
    draw_header();
}

void synth_sound_replaced(void)
{
    patch_loaded = NONE;
    modified = false;
    draw_header();
}

void synth_notice(const char *text)
{
    snprintf(notice, sizeof(notice), "%s", text);
    notice_until = esp_timer_get_time() + NOTICE_US;
    draw_header();
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
                      d == 0 ? "SECTIONS  (Group buttons)" : "release SAMPLING to close", true);
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
    /* "LOADING" on the left, the synth's name on the right (long names
     * don't fit on one display with it) */
    draw_centered(0, 0, DISPLAY_USABLE_WIDTH, 24, &font_large, "LOADING", true);
    snprintf(buf, sizeof(buf), "%s", synth->name);
    const font_t *f = display_text_width(&font_large, buf) <= DISPLAY_USABLE_WIDTH - 4 ? &font_large
                                                                                       : &font_medium;
    draw_centered(1, 0, DISPLAY_USABLE_WIDTH, 20, f, buf, true);
    draw_centered(1, 0, DISPLAY_USABLE_WIDTH, 48, &font_small, "one moment", true);
}

static void draw_page(void)
{
    if (synth_busy()) {
        return;  /* the driver's own screen */
    }
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
    if (mode == MODE_CATEGORY) {
        draw_categories();
        return;
    }
    if (mode == MODE_PADS) {
        draw_pads();
        return;
    }
    if (mode == MODE_TUTORIAL) {
        draw_tutorial();
        return;
    }
    if (mode == MODE_OVERVIEW) {
        draw_patch_overview();
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

#define PATCH_RELOAD_DELAY_US 400000
static int64_t patch_reload_at;  /* a setting may have changed the library: list it again then */
static char listed_dir[48];      /* the library directory the list was made from */

static const char *patch_dir(void)
{
    return synth->patch_dir_fn ? synth->patch_dir_fn() : synth->patch_dir;
}

static const char *common_dir(void)
{
    const char *dir = synth->common_dir_fn ? synth->common_dir_fn() : NULL;
    return dir && strcmp(dir, patch_dir()) ? dir : NULL;
}

/* The file in the library, or - if only the common directory has it - there */
static void patch_path(char *buf, size_t len, const char *file)
{
    snprintf(buf, len, "%s/%s", patch_dir(), file);
    const char *common = common_dir();
    if (common) {
        struct stat st;
        if (stat(buf, &st) != 0) {
            char other[MAX_PATH];
            snprintf(other, sizeof(other), "%s/%s", common, file);
            if (stat(other, &st) == 0) {
                snprintf(buf, len, "%s", other);
            }
        }
    }
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

static int patch_category(int idx)
{
    int n = patch_number(idx);
    int c = n >= 0 && n < MAX_PATCH_NUMBER ? category_by_number[n] : 0;
    return c <= 8 ? c : 0;
}

static bool patch_in_filter(int idx)
{
    int c = patch_category(idx);
    return category_filter == ALL_CATEGORIES || (c && (category_filter & (1 << (c - 1))));
}

static void read_categories(void)
{
    memset(category_by_number, 0, sizeof(category_by_number));
    char path[MAX_PATH];
    patch_path(path, sizeof(path), CATEGORY_FILE);
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(category_by_number, 1, sizeof(category_by_number), f) == 0) {
            ESP_LOGW(TAG, "%s is empty", path);
        }
        fclose(f);
    }
}

static void write_categories(void)
{
    char path[MAX_PATH];
    patch_path(path, sizeof(path), CATEGORY_FILE);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(category_by_number, 1, sizeof(category_by_number), f) != sizeof(category_by_number)) {
        ESP_LOGE(TAG, "writing %s failed", path);
    }
    if (f) {
        fclose(f);
    }
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
    if (synth->categories) {
        /* Display button 5 and the encoder below it step through the
         * categories */
        display_round_rect(1, 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, false, true);
        draw_centered(1, 0, COLUMN_INNER, SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, "CATEGORY", true);
        draw_text_cap(1, COLUMN_WIDTH + 4, SWITCH_BOX_Y + (SWITCH_BOX_H - font_medium.cap_height) / 2,
                      &font_medium, store_category ? synth->categories[store_category - 1] : "(none)",
                      true);
    }
    if (synth->unit_store) {
        /* The instrument's own memory: display button 8 (twice), slot on
         * encoder 8 */
        int x = 3 * COLUMN_WIDTH;
        bool armed = unit_store_armed_until != 0;
        display_round_rect(1, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, armed, true);
        draw_centered(1, x, COLUMN_INNER, SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, armed ? "SURE?" : synth->unit_store_label, !armed);
        /* The slot, large under its button (encoder 8 picks it) */
        char slot[12];
        synth->unit_slot_name(unit_slot, slot, sizeof(slot));
        draw_centered(1, x, COLUMN_INNER, VALUE_CAP_Y, &font_large, slot, true);
        if (synth->unit_slot_info) {
            char info[16] = "";
            synth->unit_slot_info(unit_slot, info, sizeof(info));
            draw_centered(1, x, COLUMN_INNER, BAR_Y, &font_tiny, info, true);
        }
        draw_text_cap(1, 4, 29, &font_small, "Encoder 8: the slot,", true);
        snprintf(buf, sizeof(buf), "%s twice writes it.", synth->unit_store_label);
        draw_text_cap(1, 4, 38, &font_small, buf, true);
    } else {
        draw_text_cap(1, 4, 29, &font_small, "SAVE or ENTER stores the current sound.", true);
        snprintf(buf, sizeof(buf), "Factory patches (below %03d) are read-only.", synth->first_user_patch);
        draw_text_cap(1, 4, 38, &font_small, buf, true);
    }
    if (store_target != NONE) {
        draw_text_cap(1, 4, 47, &font_small, synth->unit_store ? "DELETE twice removes it." :
                                                                 "DELETE twice removes this patch.", true);
    }
    int unknown = synth->unknown_count();
    if (unknown) {
        snprintf(buf, sizeof(buf), "%d params never set - saved as defaults.", unknown);
        draw_text_cap(1, 4, 56, &font_small, buf, true);
    }
}

static void enter_store(void)
{
    end_browse();
    overview = false;
    mode = MODE_STORE;
    memset(encoder_acc, 0, sizeof(encoder_acc));
    set_store_target(patch_loaded != NONE && patch_number(patch_loaded) >= synth->first_user_patch
                     ? patch_loaded : NONE);
    if (store_target == NONE) {
        /* A new patch made from a factory one, or from a sound read from the
         * unit, starts with that sound's name, not "User N" */
        char own[sizeof(store_name)];
        if (patch_loaded != NONE) {
            snprintf(store_name, sizeof(store_name), "%s", patch_title(patch_loaded));
        } else if (synth->sound_name && synth->sound_name(own, sizeof(own)) && own[0]) {
            snprintf(store_name, sizeof(store_name), "%s", own);
        }
    }
    /* The sound keeps the category of the patch it was made from */
    store_category = patch_loaded != NONE ? patch_category(patch_loaded) : 0;
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
    if (synth->categories && number < MAX_PATCH_NUMBER && category_by_number[number] != store_category) {
        category_by_number[number] = store_category;
        write_categories();
    }
    if (store_target != NONE && strcmp(patch_names[store_target], file) != 0) {
        char old[MAX_PATH];
        patch_path(old, sizeof(old), patch_names[store_target]);
        remove(old);  /* renamed */
    }
    ESP_LOGI(TAG, "stored %s", file);
    const char *warning = synth->save_warning ? synth->save_warning() : NULL;
    if (warning) {
        snprintf(notice, sizeof(notice), "SAVED.  %s", warning);
    } else {
        snprintf(notice, sizeof(notice), "SAVED  %s", file);
    }
    notice_until = esp_timer_get_time() + NOTICE_US;

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
    snprintf(notice, sizeof(notice), "DELETED  %s", patch_names[store_target]);
    notice_until = esp_timer_get_time() + NOTICE_US;
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
    if (mode == MODE_STORE && key == 7 && synth->unit_store) {
        if (unit_store_armed_until) {
            unit_store_armed_until = 0;
            synth->unit_store(unit_slot, store_name[0] ? store_name : "Untitled");
        } else {
            unit_store_armed_until = esp_timer_get_time() + DELETE_CONFIRM_US;
        }
        draw_store();
        return;
    }
    if (mode == MODE_STORE && key == 4 && synth->categories) {
        store_category = (store_category + 1) % 9;  /* 1-8, then (none) */
        draw_store();
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
    case SET_NOTE:  /* 60 = C4, as Roland and most of MIDI name it */
        snprintf(buf, len, "%s%d", names[v % 12], v / 12 - 1);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

/* Same column layout as the parameters: one setting per encoder */
/* Setup screen column -> setting index (columns 1-4: the synth's), a
 * global setting id (5-8) or NONE */
static int setup_setting_at(int col)
{
    if (col >= SETUP_SYNTH_COLS) {
        return col == SETUP_SYNTH_COLS ? MIDI_THRU_ID : col == SETUP_SYNTH_COLS + 1 ? LED_BRIGHT_ID
               : col == SETUP_SYNTH_COLS + 2 ? NOTES_IN_ID : col == SETUP_SYNTH_COLS + 3 ? HANG_NOTES_ID : NONE;
    }
    return col < synth->setting_count ? col : NONE;
}

static void draw_setup(void)
{
    char buf[48];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    snprintf(buf, sizeof(buf), "SETUP  %s", synth->name);
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, buf, true);
    draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "ALL SYNTHS", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "CONTROL closes", true);
    /* Global: MIDI thru (DIN IN -> DIN OUT, as is or to this synth's channel) */
    const char *thru = thru_names[midi_thru];
    draw_centered(1, 0, COLUMN_INNER, NAME_CAP_Y, &font_small, "MIDI THRU", true);
    const font_t *vf = display_text_width(&font_large, thru) <= COLUMN_INNER - 2 ? &font_large
                       : display_text_width(&font_medium, thru) <= COLUMN_INNER - 2 ? &font_medium
                       : &font_small;
    draw_centered(1, 0, COLUMN_INNER, VALUE_CAP_Y + (font_large.cap_height - vf->cap_height) / 2, vf, thru,
                  true);
    /* Global: all LEDs brighter, for daylight */
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", led_bright * 10);
    draw_centered(1, COLUMN_WIDTH, COLUMN_INNER, NAME_CAP_Y, &font_small, "LED BRIGHT", true);
    draw_centered(1, COLUMN_WIDTH, COLUMN_INNER, VALUE_CAP_Y, &font_large, pct, true);
    /* Global: notes from DIN IN light the pads (and pick e.g. a drum key) */
    draw_centered(1, 2 * COLUMN_WIDTH, COLUMN_INNER, NAME_CAP_Y, &font_small, "NOTES IN", true);
    draw_centered(1, 2 * COLUMN_WIDTH, COLUMN_INNER, VALUE_CAP_Y, &font_large, notes_in ? "ON" : "OFF", true);
    /* Global: a pad released after the part changed - stop its note (OFF) or leave it ringing (ON) */
    draw_centered(1, 3 * COLUMN_WIDTH, COLUMN_INNER, NAME_CAP_Y, &font_small, "HANG NOTES", true);
    draw_centered(1, 3 * COLUMN_WIDTH, COLUMN_INNER, VALUE_CAP_Y, &font_large, hang_notes ? "ON" : "OFF", true);
    if (synth->action) {
        /* The synth's one-off job, display button 8: press twice */
        int x = 3 * COLUMN_WIDTH;
        bool armed = action_armed_until != 0;
        display_round_rect(1, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, armed, true);
        draw_centered(1, x, COLUMN_INNER, SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, armed ? "SURE?" : synth->action_name, !armed);
    }
    for (int col = 0; col < SETUP_SYNTH_COLS; col++) {
        int i = setup_setting_at(col);
        if (i == NONE) {
            break;
        }
        int disp = col / 4, x = (col % 4) * COLUMN_WIDTH;
        draw_centered(disp, x, COLUMN_INNER, NAME_CAP_Y, &font_small, synth->settings[i].name, true);
        format_setting(i, buf, sizeof(buf));
        draw_centered(disp, x, COLUMN_INNER, VALUE_CAP_Y, &font_large, buf, true);
        char note[16] = "";
        if (synth->setting_note) {
            synth->setting_note(i, note, sizeof(note));
        }
        if (note[0]) {
            draw_centered(disp, x, COLUMN_INNER, BAR_Y, &font_tiny, note, true);
        }
    }
}

/* Setting i (a synth setting index, PAD_SCALE_ID or PAD_KEY_ID) turned by
 * encoder col */
static void setting_encoder(int i, int col, int delta)
{
    if (i >= PAD_LOW_ID) {
        encoder_acc[col] += delta;
        int steps = encoder_acc[col] / UNITS_SETTING;
        encoder_acc[col] -= steps * UNITS_SETTING;
        int *v = i == PAD_LOW_ID ? &pad_low : i == PAD_SCALE_ID ? &pad_scale
                 : i == PAD_KEY_ID ? &pad_key : i == PAD_CHORD_ID ? &pad_chord
                 : i == MIDI_THRU_ID ? &midi_thru : i == NOTES_IN_ID ? &notes_in : i == HANG_NOTES_ID ? &hang_notes
                 : &led_bright;
        int max = i == PAD_LOW_ID ? 112 : i == PAD_SCALE_ID ? SCALE_COUNT - 1 : i == PAD_KEY_ID ? 11
                  : i == PAD_CHORD_ID || i == NOTES_IN_ID || i == HANG_NOTES_ID ? 1 : i == MIDI_THRU_ID ? 2 : LED_BRIGHT_MAX;
        int min = i == LED_BRIGHT_ID ? LED_BRIGHT_MIN : 0;
        int nv = *v + steps;
        if (i == PAD_LOW_ID && pad_scale != 0) {
            /* In a scale, the lowest pad moves a scale note at a time */
            int moved = scale_step(lowest_pad_note(), steps);
            nv = moved < 0 ? *v : moved;
        }
        nv = nv < min ? min : nv > max ? max : nv;
        if (nv != *v) {
            *v = nv;
            settings_save_at = esp_timer_get_time() + WRITE_DELAY_US;
            maschine_set_led_gain(LED_GAIN_PCT(led_bright), LED_WHITE_PCT(led_bright));
            for (int pad = 0; pad < 16; pad++) {
                if (!pad_note_count[pad]) {
                    pad_idle(pad);
                }
            }
            draw_page();
        }
        return;
    }
    if (i < 0 || i >= synth->setting_count) {
        return;
    }
    encoder_acc[col] += delta;
    int steps = encoder_acc[col] / UNITS_SETTING;
    if (steps == 0) {
        return;
    }
    encoder_acc[col] -= steps * UNITS_SETTING;
    const synth_setting_t *st = &synth->settings[i];
    int v = setting_values[i] + steps;
    v = v < st->min ? st->min : v > st->max ? st->max : v;
    if (v == setting_values[i]) {
        return;
    }
    setting_values[i] = v;
    settings_save_at = esp_timer_get_time() + WRITE_DELAY_US;
    if (synth->patch_dir_fn) {
        /* The library may be another one now: listed again once the knob
         * rests (a long list takes most of a second) */
        patch_reload_at = esp_timer_get_time() + PATCH_RELOAD_DELAY_US;
    }
    draw_page();
}

/* Settings file: one "NAME=value" line per setting, so adding settings to
 * a driver later doesn't invalidate saved files */
static void load_global_settings(void)
{
    FILE *f = fopen(GLOBAL_SETTINGS_FILE, "r");
    char line[32];
    while (f && fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "PAD LOW=%d", &v) == 1 && v >= 0 && v <= 112) {
            pad_low = v;
        } else if (sscanf(line, "LED LEVEL=%d", &v) == 1 && v >= LED_BRIGHT_MIN && v <= LED_BRIGHT_MAX) {
            led_bright = v;
        } else if (sscanf(line, "MIDI THRU=%d", &v) == 1 && v >= 0 && v <= 2) {
            midi_thru = v;
        } else if (sscanf(line, "NOTES IN=%d", &v) == 1 && v >= 0 && v <= 1) {
            notes_in = v;
        } else if (sscanf(line, "HANG NOTES=%d", &v) == 1 && v >= 0 && v <= 1) {
            hang_notes = v;
        } else if (sscanf(line, "CHORD=%d", &v) == 1 && v >= 0 && v <= 1) {
            pad_chord = v;
        } else if (sscanf(line, "SCALE=%d", &v) == 1 && v >= 0 && v < SCALE_COUNT) {
            pad_scale = v;
        } else if (sscanf(line, "KEY=%d", &v) == 1 && v >= 0 && v < 12) {
            pad_key = v;
        }
    }
    if (f) {
        fclose(f);
    }
}

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
    f = fopen(GLOBAL_SETTINGS_FILE, "w");
    if (f) {
        fprintf(f, "PAD LOW=%d\nSCALE=%d\nKEY=%d\nMIDI THRU=%d\nLED LEVEL=%d\nCHORD=%d\nNOTES IN=%d\nHANG NOTES=%d\n",
                pad_low, pad_scale, pad_key, midi_thru, led_bright, pad_chord, notes_in, hang_notes);
        fclose(f);
    }
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
    if (patch_loaded == NONE && modified && synth->revert && synth->revert()) {
        modified = false;  /* the instrument's own sound, back as it was read */
        draw_page();
        return;
    }
    if (patch_loaded == NONE || !modified) {
        return;  /* nothing to revert */
    }
    ESP_LOGI(TAG, "reverting %s", patch_names[patch_loaded]);
    load_patch(patch_loaded);
    draw_page();
}

/* Silence everything, on every channel: multi-part synths (the D-110's
 * parts) play on several, and not every synth knows All Sound Off - the
 * D-110 only takes All Notes Off, which Hold overrides */
static void panic(void)
{
    for (int pad = 0; pad < 16; pad++) {
        for (int i = 0; i < pad_note_count[pad]; i++) {
            midi_send(0x80 | pad_channel[pad], pad_notes[pad][i], 0);
        }
    }
    for (int c = 0; c < 16; c++) {
        midi_send(0xB0 | c, 64, 0);   /* Hold off */
        midi_send(0xB0 | c, 120, 0);  /* All Sound Off */
        midi_send(0xB0 | c, 123, 0);  /* All Notes Off */
    }
    panic_flash_until = esp_timer_get_time() + PANIC_FLASH_US;
    ESP_LOGI(TAG, "panic on all channels");
}

/* ---- synth selection (Step) ---- */

static void load_settings(void);

/* One column per synth, its display button picks it; the active one is
 * filled, one asking for confirmation says SURE? */
/* Pads screen (Pad Mode): lowest note, scale and key on encoders 1-3, and
 * the pads' notes on the right display laid out like the pads, the key's
 * notes inverted */
static void draw_pads(void)
{
    static const char *const keys[12] = {"C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B"};
    char buf[16];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, "PADS  (all synths)", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "PAD MODE closes", true);
    const char *names[4] = {"PAD LOW", "PAD SCALE", "PAD KEY", "PAD CHORD"};
    int low = lowest_pad_note();  /* in a scale: the first scale note from PAD LOW */
    snprintf(buf, sizeof(buf), "%s%d", keys[low % 12], low / 12 - 1);  /* 60 = C4 */
    const char *vals[4] = {buf, scales[pad_scale].name, keys[pad_key], pad_chord ? "ON" : "OFF"};
    for (int k = 0; k < 4; k++) {
        int x = k * COLUMN_WIDTH;
        draw_centered(0, x, COLUMN_INNER, NAME_CAP_Y, &font_small, names[k], true);
        const font_t *vf = display_text_width(&font_large, vals[k]) <= COLUMN_INNER - 2 ? &font_large
                           : display_text_width(&font_medium, vals[k]) <= COLUMN_INNER - 2 ? &font_medium
                           : &font_small;
        draw_centered(0, x, COLUMN_INNER, VALUE_CAP_Y + (font_large.cap_height - vf->cap_height) / 2, vf,
                      vals[k], true);
    }
    const int cell_h = (DISPLAY_HEIGHT - HEADER_LINE_Y - 1) / 4;
    for (int pad = 0; pad < 16; pad++) {
        int x = (pad % 4) * COLUMN_WIDTH, y = HEADER_LINE_Y + 1 + (pad / 4) * cell_h;
        int note = pad_note(pad);
        bool root = note % 12 == pad_key;
        if (pad_chord) {
            chord_name(pad, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "%s%d", keys[note % 12], note / 12 - 1);
        }
        if (root) {
            display_round_rect(1, x + 2, y + 1, COLUMN_INNER - 4, cell_h - 1, true, true);
        }
        draw_centered(1, x, COLUMN_INNER, y + (cell_h - font_small.cap_height) / 2 + 1, &font_small, buf,
                      !root);
    }
}

/* Tutorial (Shift + Control): one page of the synth's special controls;
 * the buttons it's about are lit (update_button_leds) */
static void draw_tutorial(void)
{
    const synth_tip_t *tip = &synth->tips[tip_page];
    char buf[64];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    snprintf(buf, sizeof(buf), "TUTORIAL  %s  %d/%d", synth->name, tip_page + 1, synth->tip_count);
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, buf, true);
    draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "< > pages", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "CONTROL closes", true);
    draw_text_cap(0, 2, 16, &font_medium, tip->title, true);
    /* Lines 1-3 under the title, 4-7 on the right display */
    for (int i = 0; i < TIP_LINES && tip->lines[i]; i++) {
        int disp = i < 3 ? 0 : 1, y = i < 3 ? 32 + i * 11 : 16 + (i - 3) * 12;
        draw_text_cap(disp, 2, y, &font_small, tip->lines[i], true);
    }
}

/* Category screen: one box per display button, chosen ones filled (all,
 * by default), the number of patches under each; browsing only steps
 * through patches of the chosen categories */
static void draw_categories(void)
{
    int counts[9] = {0}, shown = 0;
    for (int i = 0; i < patch_count; i++) {
        counts[patch_category(i)]++;
        shown += patch_in_filter(i);
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "CATEGORIES  %d / %d patches", shown, patch_count);
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, buf, true);
    draw_text_cap(1, 1, HEADER_CAP_Y, &font_small, "ERASE all  SHIFT solo", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "ALL closes", true);
    for (int i = 0; i < 8; i++) {
        int disp = i / 4, x = (i % 4) * COLUMN_WIDTH;
        bool on = category_filter & (1 << i);
        display_round_rect(disp, x + 1, SWITCH_BOX_Y, COLUMN_INNER - 2, SWITCH_BOX_H, on, true);
        draw_centered(disp, x, COLUMN_INNER, SWITCH_BOX_Y + (SWITCH_BOX_H - font_small.cap_height) / 2,
                      &font_small, synth->categories[i], !on);
        snprintf(buf, sizeof(buf), "%d", counts[i + 1]);
        draw_centered(disp, x, COLUMN_INNER, VALUE_CAP_Y, &font_large, buf, true);
    }
}

#define SYNTH_BOX_H 26
#define SYNTH_STACK_STEP 4   /* how much of each card behind shows above the one in front */
#define SYNTH_STACK_MAX 3    /* cards drawn in a stack */
#define SYNTH_STATUS_CAP_Y 50
#define SYNTH_INDEX_CAP_Y 58

/* The slot of the selected card in its stack: cards are spread over the
 * slots when a device has more than SYNTH_STACK_MAX */
static int stack_slot(int col)
{
    int n = device_size[col], at = stack_top[col] - device_first[col];
    int layers = n < SYNTH_STACK_MAX ? n : SYNTH_STACK_MAX;
    return n > 1 ? at * (layers - 1) / (n - 1) : 0;
}

static int stack_target_y(int col) { return SWITCH_BOX_Y + stack_slot(col) * SYNTH_STACK_STEP; }

/* Every card at its slot, no sliding */
static void stack_snap(void)
{
    for (int col = 0; col < device_count; col++) {
        stack_y[col] = stack_target_y(col);
    }
}

/* Devices from synths[]: runs of consecutive synths with the same device;
 * a synth with none is a device of its own. The stacks start on the active
 * synth's card, or the one left on last. */
static void build_devices(void)
{
    if (device_count) {
        return;
    }
    for (int i = 0; i < SYNTH_COUNT && device_count < MAX_DEVICES; i++) {
        if (i && synths[i]->device && synths[i - 1]->device && !strcmp(synths[i]->device, synths[i - 1]->device)) {
            device_size[device_count - 1]++;
            continue;
        }
        device_first[device_count] = stack_top[device_count] = i;
        device_size[device_count++] = 1;
    }
    stack_snap();
}

static int device_of(int synth_index)
{
    for (int d = 0; d < device_count; d++) {
        if (synth_index >= device_first[d] && synth_index < device_first[d] + device_size[d]) {
            return d;
        }
    }
    return NONE;
}

/* The active synth's stack shows its card */
static void show_active_card(void)
{
    build_devices();
    for (int i = 0; i < SYNTH_COUNT; i++) {
        if (synths[i] == synth && device_of(i) != NONE) {
            stack_top[device_of(i)] = i;
        }
    }
    stack_snap();
}

/* A card's line, centred in the column; in the smaller font when too wide (e.g. "Wavestation") */
static void draw_card_line(int disp, int x, int y, const char *s, bool ink)
{
    const font_t *f = display_text_width(&font_small, s) > COLUMN_INNER - 4 ? &font_tiny : &font_small;
    draw_centered(disp, x, COLUMN_INNER, y + (f == &font_tiny ? 1 : 0), f, s, ink);
}

/* Text centred in the column, split at its first space onto two lines when
 * too wide for one */
static void draw_card_name(int disp, int x, int box_y, const char *label, bool ink)
{
    const char *sp = strchr(label, ' ');
    if (display_text_width(&font_small, label) > COLUMN_INNER - 6 && sp) {
        char first[24];
        snprintf(first, sizeof(first), "%.*s", (int)(sp - label), label);
        draw_card_line(disp, x, box_y + 4, first, ink);
        draw_card_line(disp, x, box_y + 14, sp + 1, ink);
    } else {
        draw_card_line(disp, x, box_y + (SYNTH_BOX_H - font_small.cap_height) / 2, label, ink);
    }
}

/* The patch overview (Grid): the driver draws both displays */
static void draw_patch_overview(void)
{
    if (po_page >= synth->overview_pages) {
        po_page = 0;
    }
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        synth->overview_draw(po_page, d);
    }
}

static void draw_synths(void)
{
    build_devices();
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    draw_text_cap(0, 1, HEADER_CAP_Y, &font_small, "SYNTH  (encoder: stack, button: open)", true);
    draw_right(1, DISPLAY_USABLE_WIDTH - 2, HEADER_CAP_Y, &font_small, "STEP closes", true);
    for (int col = 0; col < device_count; col++) {
        int i = stack_top[col], n = device_size[col], at = i - device_first[col];
        int disp = col / 4, x = (col % 4) * COLUMN_WIDTH;
        bool active = synths[i] == synth;
        int front_y = stack_y[col];
        /* The cards stay where they are, one slot (top edge) each; the
         * selected card is drawn whole at its own slot, in front, and the
         * others show behind it, the farthest first */
        int layers = n < SYNTH_STACK_MAX ? n : SYNTH_STACK_MAX, slot = stack_slot(col);
        for (int dist = layers - 1; dist > 0; dist--) {
            for (int k = 0; k < layers; k++) {
                if (abs(k - slot) == dist) {
                    int y = SWITCH_BOX_Y + k * SYNTH_STACK_STEP;
                    display_fill_rect(disp, x + 1, y, COLUMN_INNER - 2, SYNTH_BOX_H, false);
                    display_round_rect(disp, x + 1, y, COLUMN_INNER - 2, SYNTH_BOX_H, false, true);
                }
            }
        }
        display_fill_rect(disp, x + 1, front_y, COLUMN_INNER - 2, SYNTH_BOX_H, false);
        display_round_rect(disp, x + 1, front_y, COLUMN_INNER - 2, SYNTH_BOX_H, active, true);
        const synth_t *s = synths[i];
        if (i == synth_confirm) {
            draw_card_name(disp, x, front_y, "SURE?", !active);
        } else if (n > 1) {
            /* the device on top, this entry below */
            draw_card_line(disp, x, front_y + 4, s->device, !active);
            draw_card_line(disp, x, front_y + 14, s->entry, !active);
        } else {
            draw_card_name(disp, x, front_y, s->name, !active);
        }
        if (active) {
            draw_centered(disp, x, COLUMN_INNER, SYNTH_STATUS_CAP_Y, &font_small, "ACTIVE", true);
        } else if (s->is_loaded && s->is_loaded()) {
            /* e.g. the Plugiator's current plug-in: switching to it is instant */
            draw_centered(disp, x, COLUMN_INNER, SYNTH_STATUS_CAP_Y, &font_small, "LOADED", true);
        }
        if (n > 1) {
            char pos[8];
            snprintf(pos, sizeof(pos), "%d/%d", at + 1, n);
            draw_centered(disp, x, COLUMN_INNER, SYNTH_INDEX_CAP_Y, &font_tiny, pos, true);
        }
    }
    if (synth_confirm >= 0) {
        draw_text_cap(1, 4, 55, &font_small, "Unsaved edits: press again to switch", true);
    }
}

/* Patch to bring the synth back to: the remembered one, or the default */
static void send_volume(void)
{
    if (synth->volume_set) {
        synth->volume_set(synth->volume_get());  /* the driver sends it */
    } else if (synth->volume_cc) {
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

/* keep_state: when to give up waiting for the instrument's answer */
#define KEEP_STATE_WAIT_US 1000000
/* ...and when to ask: a moment after selecting it (at boot, right after
 * the USB connection came up, the first request got lost) */
#define KEEP_STATE_ASK_US 300000
static int64_t keep_state_fallback_at, keep_state_read_at;
static void restore_from_library(void);

void synth_state_received(void)
{
    keep_state_fallback_at = 0;
}

static void restore_patch(void)
{
    send_device_settings();
    if (synth->keep_state && synth->read) {
        /* Show what the instrument has, change nothing - unless it doesn't
         * answer (no MIDI IN cable): then the remembered patch after all */
        keep_state_read_at = esp_timer_get_time() + KEEP_STATE_ASK_US;
        draw_page();
        return;
    }
    restore_from_library();
}

static void restore_from_library(void)
{
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
    if (patch_count > 0) {
        load_patch(0);  /* the library's first */
        remember_at = 0;
    }
    draw_page();
}

/* Make the instrument ready (e.g. load the Plugiator plug-in), then send
 * the patch once it has had time to switch */
static void start_activation(void)
{
    if (!usb_ready) {
        return;  /* editor_connected() does it */
    }
    if (synth->activate && synth->activate()) {
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
    category_filter = ALL_CATEGORIES;
    keep_state_fallback_at = keep_state_read_at = 0;  /* the previous synth's, if any */
    memset(encoder_acc, 0, sizeof(encoder_acc));
    patch_loaded = patch_browse = NONE;
    inst_init = false;  /* instances start without patches */
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

static void setting_encoder(int i, int col, int delta);
static int setup_setting_at(int col);

/* Wide-range parameters (a step takes ACCEL_UNITS_MAX counter units or
 * less): turning fast steps further. A slow turn stays exact; beyond
 * ACCEL_FREE counts per report the step grows by one per ACCEL_PER, up
 * to ACCEL_GAIN_MAX. A jump of many reports at once (the loop was stalled)
 * is just the distance travelled and is left as it is. */
#define ACCEL_UNITS_MAX 20
#define ACCEL_FREE 12
#define ACCEL_PER 10
#define ACCEL_GAIN_MAX 6
#define ACCEL_STALL 150
static int accelerated(int d)
{
    int a = d < 0 ? -d : d;
    if (a <= ACCEL_FREE || a > ACCEL_STALL) {
        return d;
    }
    int gain = 1 + (a - ACCEL_FREE) / ACCEL_PER;
    return d * (gain > ACCEL_GAIN_MAX ? ACCEL_GAIN_MAX : gain);
}

static void editor_encoder_impl(int i, int delta)
{
    if (activating_until || synth_busy()) {
        return;  /* the synth isn't listening yet / is busy */
    }
    if (mode == MODE_OVERVIEW) {
        return;  /* the overview only shows */
    }
    if (mode == MODE_SETUP) {
        setting_encoder(setup_setting_at(i), i, delta);
        return;
    }
    if (mode == MODE_SYNTH) {
        /* The stack under this encoder: one card a step */
        if (i < device_count) {
            encoder_acc[i] += delta;
            int steps = encoder_acc[i] / UNITS_SETTING;
            encoder_acc[i] -= steps * UNITS_SETTING;
            int top = stack_top[i] + steps;
            top = top < device_first[i] ? device_first[i] : top >= device_first[i] + device_size[i]
                  ? device_first[i] + device_size[i] - 1 : top;
            if (top != stack_top[i]) {
                stack_top[i] = top;
                synth_confirm = -1;
                draw_synths();
            }
        }
        return;
    }
    if (mode == MODE_PADS) {
        static const int pad_settings[4] = {PAD_LOW_ID, PAD_SCALE_ID, PAD_KEY_ID, PAD_CHORD_ID};
        if (i < 4) {
            setting_encoder(pad_settings[i], i, delta);
        }
        return;
    }
    if (mode == MODE_STORE && i == 7 && synth->unit_store) {
        encoder_acc[i] += delta;
        int steps = encoder_acc[i] / UNIT_SLOT_UNITS;
        encoder_acc[i] -= steps * UNIT_SLOT_UNITS;
        int s = unit_slot + steps;
        s = s < 0 ? 0 : s >= synth->unit_store_slots ? synth->unit_store_slots - 1 : s;
        if (s != unit_slot) {
            unit_slot = s;
            unit_store_armed_until = 0;
            draw_store();
        }
        return;
    }
    if (mode == MODE_STORE && i == 4 && synth->categories) {
        /* Category of the patch being stored: (none), 1-8 */
        encoder_acc[i] += delta;
        int steps = encoder_acc[i] / CATEGORY_UNITS;
        encoder_acc[i] -= steps * CATEGORY_UNITS;
        int c = store_category + steps;
        c = c < 0 ? 0 : c > 8 ? 8 : c;
        if (c != store_category) {
            store_category = c;
            draw_store();
        }
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
    if (units <= ACCEL_UNITS_MAX) {
        delta = accelerated(delta);
    }
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
    int idx = patch_browse != NONE ? patch_browse : patch_loaded != NONE ? patch_loaded : -1;
    if (idx < 0 && steps < 0) {
        idx = 0;  /* nothing loaded: going back starts from the end */
    }
    /* Step over patches outside the category filter */
    int dir = steps > 0 ? 1 : -1, found = NONE;
    for (int left = steps * dir, guard = 0; left > 0 && guard < patch_count * 2; guard++) {
        idx = ((idx + dir) % patch_count + patch_count) % patch_count;
        if (patch_in_filter(idx)) {
            found = idx;
            left--;
        }
    }
    if (found == NONE) {
        return;  /* no patch in the chosen categories */
    }
    patch_browse = found;
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

/* Held arrows repeat: after a moment, and faster after a couple of seconds */
#define ARROW_REPEAT_DELAY_US 450000
#define ARROW_FAST_AFTER_US 2000000
#define ARROW_PATCH_US 100000   /* patch arrows: one patch per repeat, four when fast */
#define ARROW_PAGE_US 250000    /* page arrows: 120 ms when fast */
#define ARROW_PAGE_FAST_US 120000
static int arrow_bit = NONE;
static int64_t arrow_pressed_at, arrow_repeat_at;

static bool is_arrow(int bit)
{
    return bit == BTN_ARROW_LEFT || bit == BTN_ARROW_RIGHT || bit == BTN_ARROW_LEFT_2 || bit == BTN_ARROW_RIGHT_2;
}

static void arrow_action(int bit, bool fast)
{
    if (bit == BTN_ARROW_LEFT) {
        browse_patches(fast ? -4 : -1);  /* top arrows: patches (less often pressed by accident) */
    } else if (bit == BTN_ARROW_RIGHT) {
        browse_patches(fast ? 4 : 1);
    } else if (bit == BTN_ARROW_LEFT_2) {
        change_page(-1);  /* arrows by Enter, under the main encoder: pages */
    } else {
        change_page(1);
    }
}

static void editor_button_impl(int bit, bool down)
{
    if (!down && bit == arrow_bit) {
        arrow_bit = NONE;  /* let go: stop repeating */
    }
    if (synth_busy()) {
        /* A long job: only Erase (cancel) does anything */
        if (bit == BTN_SHIFT) {
            shift_held = down;
        } else if (down && bit == BTN_ERASE && synth->busy_cancel) {
            synth->busy_cancel();
        }
        return;
    }
    if (bit == BTN_MUTE) {
        if (down) {
            panic();
        }
        return;
    }
    if (bit == BTN_SHIFT) {
        shift_held = down;
        update_group_leds();
        return;
    }
    if (activating_until) {
        return;  /* the synth isn't listening yet */
    }
    /* Menu screens (Step, All, Pad Mode, Control): their button opens or
     * closes them, from the editor or straight from another menu; a Group
     * button goes straight back to the editor, at that section */
    if (bit == BTN_CONTROL && down && synth->tips && (shift_held || mode == MODE_TUTORIAL) &&
        (mode == MODE_EDIT || is_menu(mode))) {
        /* Shift + Control: the tutorial (again, or Control: out) */
        mode = mode == MODE_TUTORIAL ? MODE_EDIT : MODE_TUTORIAL;
        tip_page = 0;
        overview = false;
        end_browse();
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        update_group_leds();
        return;
    }
    if (mode == MODE_TUTORIAL && down &&
        (bit == BTN_ARROW_LEFT || bit == BTN_ARROW_RIGHT || bit == BTN_ARROW_LEFT_2 || bit == BTN_ARROW_RIGHT_2)) {
        int d = bit == BTN_ARROW_LEFT || bit == BTN_ARROW_LEFT_2 ? -1 : 1;
        tip_page = (tip_page + d + synth->tip_count) % synth->tip_count;
        draw_tutorial();
        update_group_leds();
        return;
    }
    int menu = bit == BTN_STEP ? MODE_SYNTH
               : bit == BTN_ALL && synth->categories ? MODE_CATEGORY
               : bit == BTN_PAD_MODE ? MODE_PADS
               : bit == BTN_GRID && synth->overview_draw ? MODE_OVERVIEW
               : bit == BTN_CONTROL ? MODE_SETUP
               : NONE;
    if (menu != NONE && down && (mode == MODE_EDIT || is_menu(mode))) {
        end_browse();
        cancel_init();
        overview = false;
        synth_confirm = -1;
        mode = mode == (ui_mode_t)menu ? MODE_EDIT : (ui_mode_t)menu;
        if (mode == MODE_SYNTH) {
            show_active_card();
        }
        memset(encoder_acc, 0, sizeof(encoder_acc));
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        return;
    }
    if (mode == MODE_OVERVIEW && down) {
        /* the instance (part, wave) can still be picked; the arrows turn the page */
        bool again = false;
        if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8 && shift_held && synth->shift_group &&
            synth->shift_group(bit - BTN_GROUP_A)) {
            again = true;
        } else if ((bit == BTN_STEP_LEFT || bit == BTN_STEP_RIGHT) && !shift_held && synth->instance_step) {
            synth->instance_step(bit == BTN_STEP_LEFT ? -1 : 1);
            again = true;
        } else if (synth->overview_pages > 1 &&
                   (bit == BTN_ARROW_LEFT || bit == BTN_ARROW_RIGHT || bit == BTN_ARROW_LEFT_2 ||
                    bit == BTN_ARROW_RIGHT_2)) {
            int d = bit == BTN_ARROW_LEFT || bit == BTN_ARROW_LEFT_2 ? -1 : 1;
            po_page = (po_page + d + synth->overview_pages) % synth->overview_pages;
            again = true;
        }
        if (again) {
            draw_patch_overview();
            update_group_leds();
            return;
        }
    }
    if (is_menu(mode) && down && bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8) {
        int section = bit - BTN_GROUP_A;
        mode = MODE_EDIT;
        memset(encoder_acc, 0, sizeof(encoder_acc));
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        if (section == synth->pages[page].section || section_first_page(section) == NONE) {
            go_to_page(page);  /* back where the editor was */
        } else {
            select_section(section);
        }
        return;
    }
    if (mode == MODE_SYNTH) {
        int col = bit - BTN_DISPLAY_1;
        if (down && col >= 0 && col < device_count) {
            pick_synth(stack_top[col]);
        }
        return;
    }
    if (mode == MODE_CATEGORY) {
        if (down && bit >= BTN_DISPLAY_1 && bit < BTN_DISPLAY_1 + 8) {
            /* Shift + a category: only that one (solo) */
            int bitmask = 1 << (bit - BTN_DISPLAY_1);
            category_filter = shift_held ? bitmask : category_filter ^ bitmask;
            draw_categories();
        } else if (down && bit == BTN_ERASE) {
            category_filter = ALL_CATEGORIES;  /* back to the default: all */
            draw_categories();
        }
        return;
    }
    if (mode == MODE_SETUP && down && bit == BTN_DISPLAY_1 + 7 && synth->action) {
        if (action_armed_until) {
            action_armed_until = 0;
            mode = MODE_EDIT;  /* the job reports in the editor's header */
            for (int d = 0; d < DISPLAY_COUNT; d++) {
                display_clear(d);
            }
            draw_page();
            synth->action();
        } else {
            action_armed_until = esp_timer_get_time() + ACTION_CONFIRM_US;
            draw_setup();
        }
        return;
    }
    if (mode == MODE_PADS || mode == MODE_SETUP || mode == MODE_TUTORIAL || mode == MODE_OVERVIEW) {
        return;
    }
    if (mode != MODE_EDIT) {
        if (down) {
            store_button(bit);
        }
        return;
    }
    if (bit == BTN_VOLUME && has_volume()) {
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
    } else if (is_arrow(bit)) {
        arrow_action(bit, false);
        arrow_bit = bit;
        arrow_pressed_at = esp_timer_get_time();
        arrow_repeat_at = arrow_pressed_at + ARROW_REPEAT_DELAY_US;
    } else if (bit == BTN_BROWSE) {
        encoder_browses = !encoder_browses;
        draw_header();
    } else if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8 && shift_held && synth->shift_group) {
        if (synth->shift_group(bit - BTN_GROUP_A)) {
            draw_page();
            update_group_leds();
        }
    } else if (bit >= BTN_GROUP_A && bit < BTN_GROUP_A + 8) {
        select_section(bit - BTN_GROUP_A);
    } else if (bit == BTN_AUTO && synth->read) {
        synth->read();
    } else if ((bit == BTN_STEP_LEFT || bit == BTN_STEP_RIGHT) &&
               (shift_held ? synth->shift_step != NULL : synth->instance_step != NULL)) {
        /* Step arrows (Shift: the driver's other step) - either may change
         * the instance (e.g. the D-110 part); each keeps its own patch */
        if (!inst_init) {
            for (int i = 0; i < MAX_INSTANCES; i++) {
                inst_loaded[i] = NONE;
            }
            inst_init = true;
        }
        int old = synth->instance ? synth->instance() % MAX_INSTANCES : 0;
        inst_loaded[old] = patch_loaded;
        inst_modified[old] = modified;
        end_browse();
        int d = bit == BTN_STEP_LEFT ? -1 : 1;
        if (shift_held) {
            synth->shift_step(d);
        } else {
            synth->instance_step(d);
        }
        int now = synth->instance ? synth->instance() % MAX_INSTANCES : 0;
        patch_loaded = inst_loaded[now];
        modified = inst_modified[now];
        draw_page();
        update_group_leds();
    } else if (bit == BTN_ENTER && patch_browse != NONE) {
        load_patch(patch_browse);
        patch_browse = NONE;
        draw_page();
    }
}

/* Shift + main encoder: this many patches per detent */
#define SHIFT_BROWSE_STEPS 8

static void editor_main_encoder_impl(int steps)
{
    if (activating_until || synth_busy()) {
        return;
    }
    if (volume_held && mode == MODE_EDIT) {
        int v = volume_value() + steps * VOLUME_STEP;
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if (synth->volume_set) {
            synth->volume_set(v);
        } else {
            setting_values[synth->volume_setting] = v;
            settings_save_at = esp_timer_get_time() + WRITE_DELAY_US;
            send_volume();
        }
        draw_header();
        return;
    }
    cancel_init();
    if (mode == MODE_STORE) {
        store_main_encoder(shift_held ? steps * SHIFT_BROWSE_STEPS : steps);
        return;
    }
    if (mode == MODE_TUTORIAL) {
        tip_page = (tip_page + (steps > 0 ? 1 : -1) + synth->tip_count) % synth->tip_count;
        draw_tutorial();
        update_group_leds();
        return;
    }
    if (mode == MODE_OVERVIEW) {
        if (!(synth->overview_scroll && synth->overview_scroll(po_page, steps)) && synth->overview_pages > 1) {
            po_page = (po_page + (steps > 0 ? 1 : -1) + synth->overview_pages) % synth->overview_pages;
        }
        draw_patch_overview();
        return;
    }
    if (mode != MODE_EDIT) {
        return;
    }
    if (encoder_browses) {
        browse_patches(shift_held ? steps * SHIFT_BROWSE_STEPS : steps);  /* Shift: faster */
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

#define STACK_ANIM_US 50000  /* a frame; the displays take no faster */
#define STACK_ANIM_PX 2      /* pixels a frame */

static void editor_tick_impl(int64_t now_us)
{
    if (mode == MODE_SYNTH && now_us >= stack_anim_at) {
        /* The selected cards slide to their slots */
        bool moved = false;
        for (int col = 0; col < device_count; col++) {
            int to = stack_target_y(col), dy = to - stack_y[col];
            if (dy) {
                stack_y[col] += dy > 0 ? (dy < STACK_ANIM_PX ? dy : STACK_ANIM_PX)
                                       : (-dy < STACK_ANIM_PX ? dy : -STACK_ANIM_PX);
                moved = true;
            }
        }
        if (moved) {
            stack_anim_at = now_us + STACK_ANIM_US;
            draw_synths();
        }
    }
    if (arrow_bit != NONE) {
        if (mode != MODE_EDIT) {
            arrow_bit = NONE;
        } else if (now_us >= arrow_repeat_at) {
            bool fast = now_us - arrow_pressed_at > ARROW_FAST_AFTER_US;
            bool patches = arrow_bit == BTN_ARROW_LEFT || arrow_bit == BTN_ARROW_RIGHT;
            arrow_action(arrow_bit, fast);
            arrow_repeat_at = now_us + (patches ? ARROW_PATCH_US : fast ? ARROW_PAGE_FAST_US : ARROW_PAGE_US);
        }
    }
    /* Follows the setting and the selected synth's channel */
    midi_set_thru((midi_thru_t)midi_thru, play_channel());
    textentry_tick(now_us);
    if (synth->tick) {
        synth->tick(now_us);
    }
    if (synth_busy()) {
        busy_shown = true;
        if (now_us >= busy_draw_at) {
            busy_draw_at = now_us + 200000;
            for (int d = 0; d < DISPLAY_COUNT; d++) {
                display_clear(d);
                synth->busy_draw(d);
            }
        }
    } else if (busy_shown) {
        busy_shown = false;
        for (int d = 0; d < DISPLAY_COUNT; d++) {
            display_clear(d);
        }
        draw_page();
        update_group_leds();
    }
    if (patch_reload_at && now_us > patch_reload_at) {
        patch_reload_at = 0;
        if (strcmp(listed_dir, patch_dir()) != 0) {
            synth_patches_changed();
        }
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
    if (keep_state_read_at && now_us > keep_state_read_at) {
        keep_state_read_at = 0;
        synth->read();
        keep_state_fallback_at = now_us + KEEP_STATE_WAIT_US;
    }
    if (keep_state_fallback_at && now_us > keep_state_fallback_at) {
        keep_state_fallback_at = 0;
        ESP_LOGI(TAG, "%s didn't answer: loading the remembered patch", synth->name);
        restore_from_library();
    }
    if (action_armed_until && now_us > action_armed_until) {
        action_armed_until = 0;
        if (mode == MODE_SETUP) {
            draw_setup();
        }
    }
    if (notice_until && now_us > notice_until) {
        notice_until = 0;
        draw_header();
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
    if (unit_store_armed_until && now_us > unit_store_armed_until) {
        unit_store_armed_until = 0;
        if (mode == MODE_STORE) {
            draw_store();
        }
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
    const char *dirs[2] = {patch_dir(), common_dir()};
    snprintf(listed_dir, sizeof(listed_dir), "%s", dirs[0]);
    for (int d = 0; d < 2; d++) {
        DIR *dir = dirs[d] ? opendir(dirs[d]) : NULL;
        if (!dir) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(dir)) && patch_count < MAX_PATCHES) {
            if (!isdigit((unsigned char)e->d_name[0])) {  /* "NNN Name"; not e.g. categories */
                continue;
            }
            bool listed = false;
            for (int i = 0; d && i < patch_count; i++) {
                listed |= strcmp(patch_names[i], e->d_name) == 0;
            }
            if (!listed) {
                patch_names[patch_count++] = strdup(e->d_name);
            }
        }
        closedir(dir);
    }
    qsort(patch_names, patch_count, sizeof(char *), cmp_names);
    read_categories();
    ESP_LOGI(TAG, "%d %s patches in %s", patch_count, synth->name, patch_dir());
}

static void load_settings(void);

/* A note from DIN IN on the synth's channel, with NOTES IN on: lights the
 * pad playing that note (if any) and tells the driver, like a pad hit */
static void note_from_din(int channel, int note, int velocity)
{
    if (!notes_in || channel != play_channel() || mode == MODE_NAME) {
        return;
    }
    for (int pad = 0; pad < 16; pad++) {
        if (pad_note(pad) == note && !pad_note_count[pad]) {
            if (velocity) {
                maschine_set_pad_led(pad, 127, 127, 127);
            } else {
                pad_idle(pad);
            }
        }
    }
    if (velocity && synth->note_played && mode == MODE_EDIT) {
        synth->note_played(note);
    }
}

/* SysEx from DIN IN goes to the active synth's driver */
static void sysex_from_din(const uint8_t *data, size_t len)
{
    if (synth->sysex_in) {
        synth->sysex_in(data, len);
    }
}

static void editor_init_impl(void)
{
    midi_set_sysex_handler(sysex_from_din);
    midi_set_note_handler(note_from_din);
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
    load_global_settings();
    maschine_set_led_gain(LED_GAIN_PCT(led_bright), LED_WHITE_PCT(led_bright));
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
    if (synth_busy()) {
        return;  /* notes would end up inside the job's SysEx on the DIN cable */
    }
    if (activating_until && down) {
        return;  /* a note now could be taken as the plug-in's confirm note */
    }
    if (mode == MODE_NAME) {
        if (down) {
            textentry_pad(pad, esp_timer_get_time());
        }
        return;
    }
    int channel = play_channel();
    if (down) {
        /* Velocity from the hit's peak pressure (main.c), over the range
         * from the press threshold (~1000) to full scale (4095) */
        int vel = 1 + (raw - 1000) * 126 / 3095;
        vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
        if (pad_chord) {
            pad_note_count[pad] = pad_chord_notes(pad, shift_held, pad_notes[pad]);
        } else {
            pad_notes[pad][0] = pad_note(pad);
            pad_note_count[pad] = 1;
        }
        pad_channel[pad] = channel;
        for (int i = 0; i < pad_note_count[pad]; i++) {
            midi_send(0x90 | channel, pad_notes[pad][i], vel);
        }
        ESP_LOGD(TAG, "pad %d: %d notes from %d, vel %d", pad, pad_note_count[pad], pad_notes[pad][0], vel);
        if (synth->note_played && mode == MODE_EDIT) {
            synth->note_played(pad_notes[pad][0]);  /* e.g. the D-110 rhythm: pick that key */
        }
        maschine_set_pad_led(pad, 127, 127, 127);
    } else {
        if (!pad_note_count[pad]) {
            return;
        }
        for (int i = 0; i < pad_note_count[pad]; i++) {
            midi_send(0x80 | (hang_notes ? channel : pad_channel[pad]), pad_notes[pad][i], 0);
        }
        pad_note_count[pad] = 0;
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
