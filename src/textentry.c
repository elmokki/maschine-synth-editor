#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "fonts.h"
#include "maschine.h"
#include "textentry.h"

#define MULTITAP_US 1000000

enum { K_CHARS, K_DEL, K_CASE, K_LEFT, K_RIGHT, K_NONE };

typedef struct {
    uint8_t kind;
    const char *chars;   /* K_CHARS: characters to cycle through */
    const char *legend;  /* shown on the right display */
} key_t;

/* Pad 0-3 = top row ... 12-15 = bottom row, like a phone keypad */
static const key_t keys[16] = {
    {K_CHARS, ".-'&!1", "1 .-'&!"}, {K_CHARS, "abc2", "2 ABC"}, {K_CHARS, "def3", "3 DEF"}, {K_DEL, NULL, "DEL"},
    {K_CHARS, "ghi4", "4 GHI"}, {K_CHARS, "jkl5", "5 JKL"}, {K_CHARS, "mno6", "6 MNO"}, {K_CASE, NULL, "Aa"},
    {K_CHARS, "pqrs7", "7 PQRS"}, {K_CHARS, "tuv8", "8 TUV"}, {K_CHARS, "wxyz9", "9 WXYZ"}, {K_LEFT, NULL, "<"},
    {K_CHARS, "()+/#", "( ) + / #"}, {K_CHARS, " 0", "0 SPACE"}, {K_NONE, NULL, ""}, {K_RIGHT, NULL, ">"},
};

static bool active;
static char text[TEXTENTRY_MAX_LEN + 1];
static int cursor;           /* insert position, 0..strlen(text) */
static bool upper = true;
static int tap_key = -1;     /* pad of a character still being cycled */
static int tap_index;
static int64_t tap_deadline;

static char apply_case(char c)
{
    return upper ? toupper((unsigned char)c) : c;
}

static void color_pads(int lit)
{
    for (int p = 0; p < 16; p++) {
        uint8_t k = keys[p].kind;
        if (p == lit) {
            maschine_set_pad_led(p, 127, 127, 127);
        } else if (k == K_CHARS) {
            maschine_set_pad_led(p, 20, 20, 20);
        } else if (k == K_NONE) {
            maschine_set_pad_led(p, 0, 0, 0);
        } else {
            maschine_set_pad_led(p, 40, 15, 0);
        }
    }
}

void textentry_start(const char *initial)
{
    snprintf(text, sizeof(text), "%s", initial);
    cursor = strlen(text);
    upper = cursor == 0;
    tap_key = -1;
    active = true;
    color_pads(-1);
}

void textentry_stop(void)
{
    active = false;
}

bool textentry_active(void)
{
    return active;
}

const char *textentry_text(void)
{
    return text;
}

static void insert(char c)
{
    int len = strlen(text);
    if (len >= TEXTENTRY_MAX_LEN) {
        return;
    }
    memmove(&text[cursor + 1], &text[cursor], len - cursor + 1);
    text[cursor++] = c;
}

void textentry_pad(int pad, int64_t now_us)
{
    const key_t *k = &keys[pad];
    bool same_tap = pad == tap_key && now_us < tap_deadline;
    if (!same_tap) {
        tap_key = -1;
    }

    switch (k->kind) {
    case K_CHARS:
        if (same_tap) {
            tap_index = (tap_index + 1) % strlen(k->chars);
            text[cursor - 1] = apply_case(k->chars[tap_index]);
        } else {
            tap_index = 0;
            int before = strlen(text);
            insert(apply_case(k->chars[0]));
            if ((int)strlen(text) == before) {
                break;  /* full */
            }
        }
        tap_key = pad;
        tap_deadline = now_us + MULTITAP_US;
        break;
    case K_DEL:
        if (cursor > 0) {
            memmove(&text[cursor - 1], &text[cursor], strlen(text) - cursor + 1);
            cursor--;
        }
        break;
    case K_CASE:
        upper = !upper;
        break;
    case K_LEFT:
        cursor = cursor > 0 ? cursor - 1 : 0;
        break;
    case K_RIGHT:
        cursor = cursor < (int)strlen(text) ? cursor + 1 : cursor;
        break;
    default:
        return;
    }
    color_pads(pad);
    textentry_draw(-1);
}

void textentry_tick(int64_t now_us)
{
    if (active && tap_key >= 0 && now_us > tap_deadline) {
        tap_key = -1;  /* character accepted */
        color_pads(-1);
        textentry_draw(-1);
    }
}

void textentry_draw(int y0)
{
    static int last_y0 = 14;
    if (y0 < 0) {
        y0 = last_y0;
    }
    last_y0 = y0;

    /* Left display: the text, large if it fits, cursor as a bar; the
     * character still being cycled is underlined */
    display_fill_rect(0, 0, y0, DISPLAY_WIDTH, DISPLAY_HEIGHT - y0, false);
    /* 20 characters of the 12 px wide large font fit the display */
    const font_t *f = &font_large;
    int x = 4, y = y0 + 8;
    char before[TEXTENTRY_MAX_LEN + 1];
    snprintf(before, sizeof(before), "%.*s", cursor, text);
    int cx = x + display_text_width(f, before);
    display_text(0, x, y, f, text, true);
    display_fill_rect(0, cx, y + 1, 2, f->height - 1, true);
    if (tap_key >= 0 && cursor > 0) {
        char last[2] = {text[cursor - 1], 0};
        int w = display_text_width(f, last);
        display_fill_rect(0, cx - w, y + f->height + 1, w, 2, true);
    }
    char info[24];
    snprintf(info, sizeof(info), "%s  %d/%d", upper ? "ABC" : "abc",
             (int)strlen(text), TEXTENTRY_MAX_LEN);
    display_text(0, DISPLAY_USABLE_WIDTH - 2 - display_text_width(&font_small, info),
                 DISPLAY_HEIGHT - font_small.height, &font_small, info, true);

    /* Right display: the pad layout */
    display_clear(1);
    for (int p = 0; p < 16; p++) {
        int cx0 = (p % 4) * 64, cy0 = (p / 4) * 16;
        if (keys[p].kind == K_NONE) {
            continue;
        }
        bool lit = p == tap_key;
        if (lit) {
            display_fill_rect(1, cx0 + 1, cy0 + 1, 61, 14, true);
        } else {
            display_rect(1, cx0 + 1, cy0 + 1, 61, 14, true);
        }
        const char *legend = keys[p].kind == K_CASE ? (upper ? "ABC > abc" : "abc > ABC")
                                                     : keys[p].legend;
        int w = display_text_width(&font_small, legend);
        display_text(1, cx0 + 1 + (61 - w) / 2, cy0 + 1 + (14 - font_small.cap_height) / 2
                     - font_small.cap_top, &font_small, legend, !lit);
    }
}
