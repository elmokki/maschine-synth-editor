#include <stdio.h>
#include <string.h>
#include "arp.h"
#include "arp_ui.h"
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "sequencer.h"

#define COLUMN_WIDTH 64
#define COLUMN_INNER 63
#define HEADER_LINE_Y 11
#define SWITCH_Y 27
#define SWITCH_H 11
#define NAME_CAP_Y 41
#define VALUE_CAP_Y 52
#define SETTINGS_FILE "/lfs/arp"
#define SETTINGS_VERSION 1
#define SAVE_DELAY_US 2000000

static const char *TAG = "arp";
static int acc[8];
static int64_t save_at;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void text_cap(int d, int x, int cap_y, const font_t *f, const char *s, bool on)
{
    display_text(d, x, cap_y - f->cap_top, f, s, on);
}

static void centered(int d, int x, int w, int cap_y, const font_t *f, const char *s, bool on)
{
    text_cap(d, x + (w - display_text_width(f, s)) / 2, cap_y, f, s, on);
}

static void touched(void)
{
    save_at = 1;  /* set to a real time by the next tick */
}

void arp_ui_init(void)
{
    arp_defaults();
    FILE *f = fopen(SETTINGS_FILE, "rb");
    if (!f) {
        return;
    }
    uint8_t b[9];
    if (fread(b, 1, sizeof b, f) == sizeof b && b[0] == SETTINGS_VERSION) {
        arp_cfg.mode = b[1] < ARP_MODES ? b[1] : ARP_UP;
        arp_cfg.rate = b[2] < ARP_RATES ? b[2] : 3;
        arp_cfg.gate = clampi(b[3], 5, 100);
        arp_cfg.octaves = clampi(b[4], 1, 4);
        arp_cfg.swing = clampi(b[5], 50, 75);
        arp_cfg.vel = clampi(b[6], 0, 127);
        arp_cfg.latch = b[7] != 0;
        arp_cfg.retrig = b[8] != 0;
    }
    fclose(f);
}

static void save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) {
        ESP_LOGE(TAG, "can't write %s", SETTINGS_FILE);
        return;
    }
    uint8_t b[9] = {SETTINGS_VERSION, arp_cfg.mode, arp_cfg.rate, arp_cfg.gate, arp_cfg.octaves, arp_cfg.swing,
                    arp_cfg.vel, arp_cfg.latch, arp_cfg.retrig};
    fwrite(b, 1, sizeof b, f);
    fclose(f);
}

void arp_ui_tick(int64_t now_us)
{
    if (save_at == 1) {
        save_at = now_us + SAVE_DELAY_US;
    } else if (save_at && now_us >= save_at) {
        save_at = 0;
        save();
    }
}

static void column(int col, const char *name, const char *value)
{
    int d = col / 4, x = (col % 4) * COLUMN_WIDTH;
    display_fill_rect(d, x, SWITCH_Y - 1, COLUMN_WIDTH, DISPLAY_HEIGHT - SWITCH_Y + 1, false);
    centered(d, x, COLUMN_INNER, NAME_CAP_Y, &font_small, name, true);
    const font_t *vf = display_text_width(&font_medium, value) <= COLUMN_INNER - 2 ? &font_medium : &font_small;
    centered(d, x, COLUMN_INNER, VALUE_CAP_Y, vf, value, true);
}

/* The switch on a column's display button, above its knob */
static void switch_box(int col, const char *name, bool on)
{
    int d = col / 4, x = (col % 4) * COLUMN_WIDTH;
    display_round_rect(d, x + 1, SWITCH_Y, COLUMN_INNER - 2, SWITCH_H, on, true);
    centered(d, x, COLUMN_INNER, SWITCH_Y + (SWITCH_H - font_small.cap_height) / 2, &font_small, name, !on);
}

static void draw_head(void)
{
    char buf[48];
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_fill_rect(d, 0, 0, DISPLAY_WIDTH, HEADER_LINE_Y + 1, false);
        display_fill_rect(d, 0, HEADER_LINE_Y, DISPLAY_USABLE_WIDTH, 1, true);
    }
    snprintf(buf, sizeof buf, "ARPEGGIATOR  %s", arp_cfg.on ? "ON" : "OFF");
    text_cap(0, 2, 1, &font_small, buf, true);
    snprintf(buf, sizeof buf, "%d BPM  SOLO: on/off", sq_pattern.bpm);
    text_cap(1, 2, 1, &font_small, buf, true);
}

void arp_ui_draw(void)
{
    char b[16];
    draw_head();
    column(0, "MODE", arp_mode_names[arp_cfg.mode]);
    column(1, "RATE", arp_rate_names[arp_cfg.rate]);
    snprintf(b, sizeof b, "%d%%", arp_cfg.gate);
    column(2, "GATE", b);
    snprintf(b, sizeof b, "%d", arp_cfg.octaves);
    column(3, "OCTAVES", b);
    snprintf(b, sizeof b, "%d%%", arp_cfg.swing);
    column(4, "SWING", b);
    if (arp_cfg.vel) {
        snprintf(b, sizeof b, "%d", arp_cfg.vel);
    } else {
        snprintf(b, sizeof b, "HELD");
    }
    column(5, "VELOCITY", b);
    column(6, "", arp_cfg.latch ? "ON" : "OFF");
    switch_box(6, "LATCH", arp_cfg.latch);
    column(7, "", arp_cfg.retrig ? "ON" : "OFF");
    switch_box(7, "RETRIG", arp_cfg.retrig);
}

void arp_ui_toggle(void)
{
    arp_cfg.on = !arp_cfg.on;
    arp_changed();
    touched();
}

bool arp_ui_button(int bit, bool down)
{
    if (bit != BTN_DISPLAY_1 + 6 && bit != BTN_DISPLAY_1 + 7) {
        return false;
    }
    if (down) {
        if (bit == BTN_DISPLAY_1 + 6) {
            arp_cfg.latch = !arp_cfg.latch;
        } else {
            arp_cfg.retrig = !arp_cfg.retrig;
        }
        arp_changed();
        touched();
        arp_ui_draw();
    }
    return true;
}

void arp_ui_encoder(int i, int delta)
{
    static const int units[8] = {60, 60, 20, 80, 12, 8, 60, 60};
    acc[i] += delta;
    int n = acc[i] / units[i];
    if (!n) {
        return;
    }
    acc[i] -= n * units[i];
    switch (i) {
    case 0:
        arp_cfg.mode = clampi(arp_cfg.mode + n, 0, ARP_MODES - 1);
        break;
    case 1:
        arp_cfg.rate = clampi(arp_cfg.rate + n, 0, ARP_RATES - 1);
        break;
    case 2:
        arp_cfg.gate = clampi(arp_cfg.gate + n * 5, 5, 100);
        break;
    case 3:
        arp_cfg.octaves = clampi(arp_cfg.octaves + n, 1, 4);
        break;
    case 4:
        arp_cfg.swing = clampi(arp_cfg.swing + n, 50, 75);
        break;
    case 5:
        arp_cfg.vel = clampi(arp_cfg.vel + n, 0, 127);
        break;
    case 6:
        arp_cfg.latch = n > 0;
        break;
    default:
        arp_cfg.retrig = n > 0;
        break;
    }
    arp_changed();
    touched();
    arp_ui_draw();
}

void arp_ui_main_encoder(int steps)
{
    sq_set_bpm(sq_pattern.bpm + steps);
    draw_head();
}
