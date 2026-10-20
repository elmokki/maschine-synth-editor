#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "fonts.h"
#include "jv880_common.h"
#include "storage.h"

static void put_addr(uint8_t *p, uint32_t v)
{
    p[0] = v >> 21 & 0x7F;
    p[1] = v >> 14 & 0x7F;
    p[2] = v >> 7 & 0x7F;
    p[3] = v & 0x7F;
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += p[i];
    }
    return (128 - (sum & 0x7F)) & 0x7F;
}

static size_t header(int dev, uint8_t command, uint32_t addr, uint8_t *out)
{
    out[0] = 0xF0;
    out[1] = 0x41;
    out[2] = dev & 0x7F;
    out[3] = JV880_MODEL_ID;
    out[4] = command;
    put_addr(out + 5, addr);
    return 9;
}

size_t jv880_dt1(int dev, uint32_t addr, const uint8_t *data, size_t len, uint8_t *out)
{
    size_t n = header(dev, 0x12, addr, out);
    for (size_t i = 0; i < len; i++) {
        out[n++] = data[i] & 0x7F;
    }
    out[n] = checksum(out + 5, n - 5);
    n++;
    out[n++] = 0xF7;
    return n;
}

size_t jv880_rq1(int dev, uint32_t addr, uint32_t size, uint8_t *out)
{
    size_t n = header(dev, 0x11, addr, out);
    put_addr(out + n, size);
    n += 4;
    out[n] = checksum(out + 5, n - 5);
    n++;
    out[n++] = 0xF7;
    return n;
}

int jv880_parse_dt1(const uint8_t *d, size_t len, int dev, uint32_t *addr, const uint8_t **data)
{
    /* F0 41 dev 46 12 a a a a data... sum F7 */
    if (len < 11 || d[0] != 0xF0 || d[1] != 0x41 || d[2] != (dev & 0x7F) || d[3] != JV880_MODEL_ID ||
        d[4] != 0x12 || d[len - 1] != 0xF7) {
        return -1;
    }
    int sum = 0;
    for (size_t i = 5; i < len - 1; i++) {
        sum += d[i];
    }
    if (sum & 0x7F) {
        return -1;
    }
    *addr = JV880_ADDR(d[5], d[6], d[7], d[8]);
    *data = d + 9;
    return (int)len - 11;
}

/* ---- expansion boards ---- */

/* The SR-JV80 series, shortened to fit under the setting (12 characters);
 * the CARD setting is an index here, board numbers 97-99 follow 19 */
const char *const jv880_card_names[23] = {
    "NO BOARD", "Pop", "Orchestral", "Piano", "Vintage Syn", "World", "Dance", "Super Sound",
    "60s/70s Keys", "Session", "Bass & Drum", "Techno", "HipHop", "Vocal", "Asia", "Special FX",
    "Orchestral2", "Country", "Latin", "House", "97 Exp. III", "98 Exp. II", "99 Exp. I",
};

int jv880_card_number(int setting) { return setting <= 19 ? setting : 97 + (setting - 20); }

/* ---- pictures ---- */

#define BAND_Y 14
#define BAND_H 38

void jv880_draw_eg_text(int disp, int x, const char *name, const char *value)
{
    char text[16];
    snprintf(text, sizeof(text), "%s %s", name, value);
    int tw = display_text_width(&font_small, text);
    display_text(disp, x + (63 - tw) / 2, JV880_EG_TEXT_CAP_Y - font_small.cap_top, &font_small, text, true);
}

/* See the header: an envelope over both displays: points at levels lv[] (0-100), reached
 * after times tm[] (0-127, wider the slower); a held sustain segment after
 * point sus_at; centre line for the pitch envelope */
void jv880_draw_env_ex(int disp, const int *lv, const int *tm, int n, int sus_at, bool centred, bool dotted,
                       bool clear)
{
    if (clear) {
        display_fill_rect(disp, 0, BAND_Y, 256, BAND_H, false);
        if (centred) {
            for (int x = 0; x < 255; x += 4) {
                display_pixel(disp, x, BAND_Y + BAND_H / 2, true);
            }
        }
    }
    const int sus_w = 40;
    int total = sus_w;
    for (int i = 1; i < n; i++) {
        total += 6 + tm[i];
    }
    float scale = 505.0f / total;
    float x = 2;
    for (int i = 1; i < n; i++) {
        float w = (6 + tm[i]) * scale;
        int x0 = (int)x, x1 = (int)(x + w);
        int y0 = BAND_Y + BAND_H - 1 - lv[i - 1] * (BAND_H - 1) / 100;
        int y1 = BAND_Y + BAND_H - 1 - lv[i] * (BAND_H - 1) / 100;
        /* only the part of the line on this display */
        for (int px = x0; px <= x1; px++) {
            if (px / 256 != disp || px % 256 > 254) {
                continue;
            }
            int py = x1 > x0 ? y0 + (y1 - y0) * (px - x0) / (x1 - x0) : y1;
            int pyn = x1 > x0 && px < x1 ? y0 + (y1 - y0) * (px + 1 - x0) / (x1 - x0) : py;
            int a = py < pyn ? py : pyn, b = py < pyn ? pyn : py;
            for (int yy = a; yy <= b; yy++) {
                if (!dotted || ((px + yy) & 1) == 0) {
                    display_pixel(disp, px % 256, yy, true);
                }
            }
        }
        x += w;
        if (i == sus_at) {
            int xs = (int)x, xe = (int)(x + sus_w * scale);
            for (int px = xs; px <= xe; px++) {
                if (px / 256 == disp && px % 256 < 255 && (!dotted || !(px & 1))) {
                    display_pixel(disp, px % 256, y1, true);
                }
            }
            x += sus_w * scale;
        }
    }
}

void jv880_draw_env(int disp, const int *lv, const int *tm, int n, int sus_at, bool centred)
{
    jv880_draw_env_ex(disp, lv, tm, n, sus_at, centred, false, true);
}


/* A small envelope for the overviews: points 0..n-1 at levels lv[] (0-100), reached after times tm[] (0-127,
 * wider the slower), a held segment after point sus_at (-1 = none) */
void jv880_draw_env_mini(int disp, int x, int y, int w, int h, const int *lv, const int *tm, int n, int sus_at)
{
    const int sus_w = 24;
    int total = sus_at >= 0 ? sus_w : 0;
    for (int i = 1; i < n; i++) {
        total += 6 + tm[i];
    }
    int px = x, py = y + h - 1 - lv[0] * (h - 1) / 100, pos = 0;
    for (int i = 1; i < n; i++) {
        pos += 6 + tm[i];
        int nx = x + pos * (w - 1) / total, ny = y + h - 1 - lv[i] * (h - 1) / 100;
        display_line(disp, px, py, nx, ny, true);
        px = nx;
        py = ny;
        if (i == sus_at) {
            pos += sus_w;
            nx = x + pos * (w - 1) / total;
            display_line(disp, px, py, nx, py, true);
            px = nx;
        }
    }
}

/* ---- waveforms ---- */

/* The name list in memory (255 lines of 16 characters + newline): reading
 * a file while a knob is turned stalls the editor long enough to lose count
 * of the knob. list 0 = the unit's own, else the board's number. */
#define WAVE_LINE (JV880_WAVE_NAME_LEN + 1)
static char wave_lines[255 * WAVE_LINE];
static int wave_list_loaded = -1, wave_list_count;
static bool wave_list_ok;

static bool wave_list(int list)
{
    if (list == wave_list_loaded) {
        return wave_list_ok;
    }
    wave_list_loaded = list;
    wave_list_ok = false;
    wave_list_count = 255;
    char path[40];
    if (list == 0) {
        snprintf(path, sizeof(path), STORAGE_ROOT "/jv880/waves/int");
    } else {
        snprintf(path, sizeof(path), STORAGE_ROOT "/jv880/waves/card%02d", list);
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        wave_list_ok = fread(wave_lines, 1, sizeof(wave_lines), f) == sizeof(wave_lines);
        fclose(f);
    }
    if (wave_list_ok) {
        for (int n = 254; n >= 0; n--) {
            const char *line = &wave_lines[n * WAVE_LINE];
            int i = 0;
            while (i < JV880_WAVE_NAME_LEN && line[i] == ' ') {
                i++;
            }
            if (i < JV880_WAVE_NAME_LEN) {
                wave_list_count = n + 1;
                break;
            }
        }
    }
    return wave_list_ok;
}

/* How many waves the group has: the unit's own 129, a board's as many as
 * its list names (the unit ignores numbers past that), else 255 */
int jv880_wave_count(int group, int card)
{
    if (group == 0) {
        return 129;
    }
    if (group != 1 || card <= 0) {
        return 255;
    }
    return wave_list(card) ? wave_list_count : 255;
}

/* The wave's name from its list, when there is one */
bool jv880_wave_name(int group, int number, int card, char *buf, size_t len)
{
    int list = group == 0 ? 0 : group == 1 && card > 0 ? card : -1;
    if (list < 0 || number < 0 || number > 254 || !wave_list(list)) {
        return false;
    }
    char name[JV880_WAVE_NAME_LEN + 1];
    memcpy(name, &wave_lines[number * WAVE_LINE], JV880_WAVE_NAME_LEN);
    name[JV880_WAVE_NAME_LEN] = 0;
    for (int i = JV880_WAVE_NAME_LEN - 1; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(buf, len, "%s", name);
    return name[0] != 0;
}

/* Does the wave exist: inside the group, and named in its list (when it
 * has one - the lists have no gaps to speak of, so this mostly just
 * checks the count) */
bool jv880_wave_exists(int group, int number, int card)
{
    if (number < 0 || number >= jv880_wave_count(group, card)) {
        return false;
    }
    char name[JV880_WAVE_NAME_LEN + 1];
    bool listed = group == 0 || (group == 1 && card > 0);
    return !listed || !wave_list(group == 0 ? 0 : card) || jv880_wave_name(group, number, card, name, sizeof(name));
}

/* The wave in the group nearest to number (the lower one on a tie) */
int jv880_wave_nearest(int group, int number, int card)
{
    int last = jv880_wave_count(group, card) - 1;
    number = number < 0 ? 0 : number > last ? last : number;
    for (int d = 0; d <= last; d++) {
        if (number - d >= 0 && jv880_wave_exists(group, number - d, card)) {
            return number - d;
        }
        if (number + d <= last && jv880_wave_exists(group, number + d, card)) {
            return number + d;
        }
    }
    return number;
}

/* Text with its caps starting at cap_y, centred in the column */
static void draw_cap_centred(int disp, int x, int w, int cap_y, const font_t *font, const char *s)
{
    display_text(disp, x + (w - display_text_width(font, s)) / 2, cap_y - font->cap_top, font, s, true);
}

/* A name that fits the column in the largest font that allows, or on two
 * lines of the small one, split at a space near the middle */
static void draw_wave_name(int disp, int x, int w, int cap_y, const char *name)
{
    int limit = w - 2;
    if (display_text_width(&font_small, name) <= limit) {
        draw_cap_centred(disp, x, w, cap_y, &font_small, name);
    } else if (display_text_width(&font_tiny, name) <= limit) {
        draw_cap_centred(disp, x, w, cap_y + 1, &font_tiny, name);
    } else {
        int len = strlen(name), cut = len / 2, best = 99;
        for (int i = 1; i < len; i++) {
            if (name[i] == ' ' && abs(len - 2 * i) < best) {
                best = abs(len - 2 * i);
                cut = i;
            }
        }
        char top[JV880_WAVE_NAME_LEN + 1], bottom[JV880_WAVE_NAME_LEN + 1];
        snprintf(top, sizeof(top), "%.*s", cut, name);
        snprintf(bottom, sizeof(bottom), "%s", name + cut + (name[cut] == ' '));
        draw_cap_centred(disp, x, w, cap_y - 2, &font_tiny, top);  /* caps 51-56 */
        draw_cap_centred(disp, x, w, cap_y + 5, &font_tiny, bottom);  /* caps 58-63 */
    }
}

/* A code big and its name below (when there is one) in a column */
void jv880_draw_code_name(int disp, int x, int y, int w, const char *code, const char *name)
{
    draw_cap_centred(disp, x, w, y + 1, &font_medium, code);
    if (name && name[0]) {
        draw_wave_name(disp, x, w, y + 15, name);
    }
}

/* The wave number, and its name below when there is one; never a bar */
void jv880_draw_wave(int disp, int x, int y, int w, const char *num, int group, int number, int card)
{
    char name[JV880_WAVE_NAME_LEN + 1];
    if (!jv880_wave_name(group, number, card, name, sizeof(name))) {
        name[0] = 0;
    }
    jv880_draw_code_name(disp, x, y, w, num, name);
}
