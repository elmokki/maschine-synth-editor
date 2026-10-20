#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "plugiator.h"

#define MAX_PATCH_BYTES 512
#define PLUGIN_NOTE_DELAY_MS 100

/* Encoder counter units per value step: ~1300 units per turn, so
 * continuous values move ~130 per turn and position knobs one position per
 * ~1/16 turn */
#define UNITS_CONT 10
#define UNITS_STEPS 80
#define UNITS_RAW24 50

static const plug_param_t *prm(plug_set_t *s, int p)
{
    return &s->params[p];
}

void plug_init_values(plug_set_t *s)
{
    if (s->initialised) {
        return;
    }
    for (int p = 0; p < s->count; p++) {
        s->values[p] = prm(s, p)->default_value;
    }
    s->initialised = true;
}

static void send(plug_set_t *s, int p)
{
    uint8_t status = (prm(s, p)->msg == PLUG_CC ? 0xB0 : 0xA0) | synth_setting(SETTING_CHANNEL);
    midi_send(status, prm(s, p)->number, s->values[p]);
}

void plug_set(plug_set_t *s, int p, uint8_t v)
{
    s->values[p] = v & 0x7F;
    s->known[p] = true;
    send(s, p);
}

int plug_step_pos(const plug_param_t *pp, uint8_t v)
{
    int best = 0;
    for (int i = 1; i < pp->label_count; i++) {
        if (abs(pp->step_values[i] - v) < abs(pp->step_values[best] - v)) {
            best = i;
        }
    }
    return best;
}

/* The knob's own direction: reversed knobs send 127 at their minimum */
static int knob_value(const plug_param_t *pp, uint8_t v)
{
    return pp->reverse ? 127 - v : v;
}

/* ---- display ---- */

const char *plug_name(plug_set_t *s, int p)
{
    return prm(s, p)->short_name;
}

/* Value on the plug-in GUI's scale, in tenths: a range symmetric around 0
 * puts 0 at 64, anything else is linear over 0-127 */
static int scaled_tenths(const plug_param_t *pp, int v)
{
    int lo = pp->disp_lo * 10, hi = pp->disp_hi * 10;
    int num, den;
    if (lo == -hi) {
        num = (v - 64) * hi;
        den = v >= 64 ? 63 : 64;
    } else {
        num = lo * 127 + v * (hi - lo);
        den = 127;
    }
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

static void format_scaled(const plug_param_t *pp, int v, char *buf, size_t len)
{
    int t = scaled_tenths(pp, v);
    const char *sign = t < 0 ? "-" : pp->disp_lo < 0 && t > 0 ? "+" : "";
    int a = t < 0 ? -t : t;
    const char *unit = pp->unit ? pp->unit : "";
    if (pp->decimals) {
        snprintf(buf, len, "%s%d.%d%s", sign, a / 10, a % 10, unit);
    } else {
        snprintf(buf, len, "%s%d%s", sign, (a + 5) / 10, unit);
    }
}

/* Time of a knob with a measured curve: logarithmic between the two nearest
 * points, extended past the ends with the end segments' slope */
static float curve_ms(const plug_curve_t *c, int v)
{
    int i = 1;
    while (i < c->count - 1 && v > c->points[i].value) {
        i++;
    }
    const plug_curve_point_t *a = &c->points[i - 1], *b = &c->points[i];
    float t = (float)(v - a->value) / (b->value - a->value);
    return a->ms * powf((float)b->ms / a->ms, t);
}

float plug_time_ms(const plug_param_t *pp, int v) { return curve_ms(pp->time_curve, v); }

static void format_time(const plug_curve_t *c, int v, char *buf, size_t len)
{
    float ms = curve_ms(c, v);
    if (ms < 1000.0f) {
        snprintf(buf, len, "%dms", (int)(ms + 0.5f));
    } else if (ms < 10000.0f) {
        int tenths = (int)(ms / 100.0f + 0.5f);
        snprintf(buf, len, "%d.%ds", tenths / 10, tenths % 10);
    } else {
        snprintf(buf, len, "%ds", (int)(ms / 1000.0f + 0.5f));
    }
}

void plug_format(plug_set_t *s, int p, char *buf, size_t len)
{
    static const char *const notes[12] = {"C", "C#", "D", "D#", "E", "F",
                                          "F#", "G", "G#", "A", "A#", "B"};
    const plug_param_t *pp = prm(s, p);
    uint8_t v = s->values[p];
    int kv = knob_value(pp, v);
    if (!s->known[p]) {
        snprintf(buf, len, "--");
    } else if (pp->type == PT_SWITCH) {
        snprintf(buf, len, "%s", pp->labels ? pp->labels[v >= 64] : v >= 64 ? "ON" : "OFF");
    } else if (pp->type == PT_STEPS) {
        int pos = plug_step_pos(pp, v);
        if (pp->labels) {
            snprintf(buf, len, "%s", pp->labels[pos]);
        } else {
            snprintf(buf, len, "%d", pos);
        }
    } else if (pp->type == PT_NOTE) {  /* 60 = C4, as everywhere in the editor */
        snprintf(buf, len, "%s%d", notes[v % 12], v / 12 - 1);
    } else if (pp->type == PT_MIX) {
        /* Ends by name, in between as a ratio of the two, e.g. 70/30 */
        int b = (kv * 100 + 63) / 127;
        if (b == 0 || b == 100) {
            snprintf(buf, len, "%s", pp->labels[b == 100]);
        } else {
            snprintf(buf, len, "%d/%d", 100 - b, b);
        }
    } else if (pp->type == PT_LIST) {
        snprintf(buf, len, "%s", pp->labels[v < pp->label_count ? v : pp->label_count - 1]);
    } else if (pp->log_k) {
        float k = pp->log_k;
        float d = pp->log_lo + (pp->log_hi - pp->log_lo) * (powf(k, kv / 127.0f) - 1.0f) / (k - 1.0f);
        snprintf(buf, len, d < 10.0f ? "%.2f" : "%.1f", d);
    } else if (pp->time_curve) {
        format_time(pp->time_curve, kv, buf, len);
    } else if (pp->disp_lo != pp->disp_hi) {
        format_scaled(pp, kv, buf, len);
    } else {
        snprintf(buf, len, "%d", kv);
    }
}

int plug_bar(plug_set_t *s, int p)
{
    const plug_param_t *pp = prm(s, p);
    if (!s->known[p] || pp->type == PT_SWITCH) {
        return -1;
    }
    uint8_t v = s->values[p];
    switch (pp->type) {
    case PT_STEPS:
        return plug_step_pos(pp, v) * 1000 / (pp->label_count - 1);
    case PT_LIST:
        return (v < pp->label_count ? v : pp->label_count - 1) * 1000 / (pp->label_count - 1);
    case PT_RAW24:
        return (v > 24 ? 24 : v) * 1000 / 24;
    default:
        return knob_value(pp, v) * 1000 / 127;
    }
}

bool plug_on(plug_set_t *s, int p)
{
    return s->known[p] && s->values[p] >= 64;
}

/* ---- editing ---- */

int plug_units(plug_set_t *s, int p)
{
    switch (prm(s, p)->type) {
    case PT_STEPS:
    case PT_LIST:
        return UNITS_STEPS;
    case PT_RAW24:
        return UNITS_RAW24;
    default:
        return UNITS_CONT;
    }
}

bool plug_step(plug_set_t *s, int p, int steps)
{
    plug_init_values(s);
    const plug_param_t *pp = prm(s, p);
    /* Unknown value: start from the factory default instead of guessing */
    int v = s->known[p] ? s->values[p] : pp->default_value;
    switch (pp->type) {
    case PT_STEPS: {
        int pos = plug_step_pos(pp, v) + steps;
        pos = pos < 0 ? 0 : pos >= pp->label_count ? pp->label_count - 1 : pos;
        v = pp->step_values[pos];
        break;
    }
    case PT_SWITCH:
        v = steps > 0 ? 127 : 0;
        break;
    default: {
        int max = pp->type == PT_RAW24 ? 24 : pp->type == PT_LIST ? pp->label_count - 1 : 127;
        v += pp->reverse ? -steps : steps;
        v = v < 0 ? 0 : v > max ? max : v;
        break;
    }
    }
    if (s->known[p] && v == s->values[p]) {
        return false;
    }
    plug_set(s, p, v);
    return true;
}

void plug_toggle(plug_set_t *s, int p)
{
    /* An unknown switch is drawn as off, so its first press turns it on -
     * toggling from the factory default could look like nothing happened */
    plug_set(s, p, s->known[p] && s->values[p] >= 64 ? 0 : 127);
}

/* ---- pictures ---- */

/* Two cycles of a Minimoog waveform (position 0-5 of the waveform knob) */
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

void plug_draw_wave(int disp, int x, int y, int w, int h, int pos) { draw_wave(disp, x, y, w, h, pos); }

/* An organ drawbar pulled out to position 0-8, as on the instrument: a
 * slot at the top, the shaft out of it and the handle at its end, the
 * handle in the drawbar's colour (brown = dotted, white = outlined,
 * black = filled); the position number beside it. */
static void draw_drawbar(int disp, int x, int y, int w, int h, int pos, plug_icon_t colour)
{
    const int handle_w = 13, handle_h = 7, step = (h - handle_h - 3) / 8;
    int cx = x + w / 2 - 8;
    int hx = cx - handle_w / 2;
    display_fill_rect(disp, hx - 2, y, handle_w + 4, 1, true);  /* the slot */
    int shaft = 1 + pos * step;
    display_rect(disp, cx - 1, y + 1, 3, shaft, true);
    int hy = y + 1 + shaft;
    switch (colour) {
    case PI_DRAWBAR_BLACK:
        display_round_rect(disp, hx, hy, handle_w, handle_h, true, true);
        break;
    case PI_DRAWBAR_BROWN:
        display_round_rect(disp, hx, hy, handle_w, handle_h, false, true);
        for (int yy = hy + 2; yy < hy + handle_h - 2; yy++) {
            for (int xx = hx + 2 + (yy & 1); xx < hx + handle_w - 2; xx += 2) {
                display_pixel(disp, xx, yy, true);
            }
        }
        break;
    default:
        display_round_rect(disp, hx, hy, handle_w, handle_h, false, true);
        break;
    }
    char num[4];
    snprintf(num, sizeof(num), "%d", pos);
    display_text(disp, cx + 12, y + (h - font_medium.height) / 2, &font_medium, num, true);
}

int plug_draw(plug_set_t *s, int p, int disp, int x, int y, int w, int h)
{
    const plug_param_t *pp = prm(s, p);
    if (!s->known[p] || pp->icon == PI_NONE) {
        return SYNTH_DRAW_NONE;
    }
    int pos = pp->type == PT_STEPS ? plug_step_pos(pp, s->values[p]) : 0;
    if (pp->icon == PI_WAVE) {
        draw_wave(disp, x + 9, y, w - 18, font_large.cap_height, pos);
        return SYNTH_DRAW_LABEL;
    }
    draw_drawbar(disp, x, y, w, h, pos, pp->icon);
    return SYNTH_DRAW_FULL;
}

/* ---- patches ---- */

static int find_param(plug_set_t *s, uint8_t status, uint8_t number)
{
    plug_msg_t msg = (status & 0xF0) == 0xB0 ? PLUG_CC : PLUG_PAT;
    for (int p = 0; p < s->count; p++) {
        if (prm(s, p)->msg == msg && prm(s, p)->number == number) {
            return p;
        }
    }
    return SYNTH_NONE;
}

static size_t read_patch(const char *path, uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n;
}

bool plug_patch_load(plug_set_t *s, const char *path)
{
    plug_init_values(s);
    uint8_t buf[MAX_PATCH_BYTES];
    size_t n = read_patch(path, buf, sizeof(buf));
    if (n == 0) {
        ESP_LOGE(s->tag, "can't read %s", path);
        return false;
    }
    for (size_t i = 0; i + 3 <= n; i += 3) {
        uint8_t type = buf[i] & 0xF0;
        if (type != 0xB0 && type != 0xA0) {
            continue;
        }
        midi_send(type | synth_setting(SETTING_CHANNEL), buf[i + 1], buf[i + 2]);
        int p = find_param(s, type, buf[i + 1]);
        if (p != SYNTH_NONE) {
            s->values[p] = buf[i + 2];
            s->known[p] = true;
        }
    }
    return true;
}

bool plug_patch_save(plug_set_t *s, const char *path)
{
    /* Same format as the imported presets: every in-preset parameter, in
     * table order; unknown ones as their factory default */
    uint8_t buf[MAX_PATCH_BYTES];
    size_t n = 0;
    for (int p = 0; p < s->count && n + 3 <= sizeof(buf); p++) {
        if (!prm(s, p)->in_preset) {
            continue;
        }
        buf[n++] = prm(s, p)->msg == PLUG_CC ? 0xB0 : 0xA0;
        buf[n++] = prm(s, p)->number;
        buf[n++] = s->known[p] ? s->values[p] : prm(s, p)->default_value;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

int plug_unknown_count(plug_set_t *s)
{
    int n = 0;
    for (int p = 0; p < s->count; p++) {
        n += prm(s, p)->in_preset && !s->known[p];
    }
    return n;
}

/* Values of the default (init) patch, read from its file on first use */
void plug_read_init_values(plug_set_t *s)
{
    if (s->init_read) {
        return;
    }
    s->init_read = true;
    for (int p = 0; p < s->count; p++) {
        s->init[p] = prm(s, p)->default_value;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s->patch_dir, s->default_patch);
    uint8_t buf[MAX_PATCH_BYTES];
    size_t n = read_patch(path, buf, sizeof(buf));
    if (n == 0) {
        ESP_LOGW(s->tag, "no %s, value init uses factory defaults", path);
        return;
    }
    for (size_t i = 0; i + 3 <= n; i += 3) {
        int p = find_param(s, buf[i] & 0xF0, buf[i + 1]);
        if (p != SYNTH_NONE) {
            s->init[p] = buf[i + 2];
        }
    }
}

bool plug_reset(plug_set_t *s, int p)
{
    plug_init_values(s);
    plug_read_init_values(s);
    if (s->known[p] && s->values[p] == s->init[p]) {
        return false;
    }
    plug_set(s, p, s->init[p]);
    return true;
}

/* The plug-in the editor loaded last: "<tag> <program>" */
#define LOADED_FILE STORAGE_ROOT "/state/plugiator_loaded"
static char loaded[32];
static bool loaded_read;

static const char *loaded_plugin(void)
{
    if (!loaded_read) {
        loaded_read = true;
        FILE *f = fopen(LOADED_FILE, "r");
        if (f) {
            if (!fgets(loaded, sizeof(loaded), f)) {
                loaded[0] = '\0';
            }
            fclose(f);
        }
    }
    return loaded;
}

bool plug_is_loaded(plug_set_t *s)
{
    size_t n = strlen(s->tag);
    return strncmp(loaded_plugin(), s->tag, n) == 0 && loaded[n] == ' ';
}

bool plug_activate(plug_set_t *s)
{
    char want[32];
    snprintf(want, sizeof(want), "%s %d", s->tag, synth_setting(SETTING_PLUGIN_PC));
    if (strcmp(loaded_plugin(), want) == 0) {
        ESP_LOGI(s->tag, "plug-in already loaded");
        return false;
    }
    snprintf(loaded, sizeof(loaded), "%s", want);
    FILE *f = fopen(LOADED_FILE, "w");
    if (f) {
        fputs(loaded, f);
        fclose(f);
    }
    /* The Plugiator loads a plug-in on a Program Change followed by
     * any note within a short time (tested from the PC: a velocity-64 note
     * 50 ms later works and isn't heard; the new plug-in plays ~1 s later).
     * Sending the note right with the Program Change switched MiniMax ->
     * B4000 but not back, so the note waits a moment here. Blocking the USB
     * loop for that is harmless: the Program Change is already on its way
     * and the note simply goes out after it. */
    int ch = synth_setting(SETTING_CHANNEL);
    int program = synth_setting(SETTING_PLUGIN_PC);
    midi_send(0xC0 | ch, program, 0);
    vTaskDelay(pdMS_TO_TICKS(PLUGIN_NOTE_DELAY_MS));
    midi_send(0x90 | ch, 60, 64);
    midi_send(0x80 | ch, 60, 0);
    ESP_LOGI(s->tag, "loading plug-in (program %d)", program);
    return true;
}
