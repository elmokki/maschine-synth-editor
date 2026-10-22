/*
 * Maschine Mk2 outputs owned by main.c (the HID interrupt OUT pipe), for
 * use by the UI code. Same threading rule as everything else: call from
 * the USB client event loop.
 */
#pragma once
#include <stdint.h>

/* Pad 0-15 LED (pad numbering as in docs/PROTOCOL.md: 0-3 top row left to
 * right ... 12-15 bottom row), RGB 0-127. */
void maschine_set_pad_led(int pad, uint8_t r, uint8_t g, uint8_t b);

/* Group button A-H (0-7) LED, RGB 0-127. Sent with the next OUT slot and
 * re-sent automatically after a reconnect. */
void maschine_set_group_led(int group, uint8_t r, uint8_t g, uint8_t b);

/* Same, with the button's left and right LEDs set separately (rgb[3]
 * each); the diffuser blends them into a left-to-right gradient. */
void maschine_set_group_led_lr(int group, const uint8_t left[3], const uint8_t right[3]);

/* Mono LED of the button with this report 0x01 input bit (see the button
 * table in docs/PROTOCOL.md), brightness 0-127. Every button except the
 * Group buttons (RGB, above) has one. */
void maschine_set_button_led(int bit, uint8_t value);

/* Every LED above is scaled by this, in percent (100 = as asked, more for
 * daylight; off stays off). An RGB colour that can't get brighter without
 * clipping keeps its hue at full and gets the rest of its brightness as
 * white, at most max_white_pct of the way. */
void maschine_set_led_gain(int pct, int max_white_pct);
/* The raw pad value (0-4095) a pad counts as pressed from (it lets go at 3/5 of it) */
void maschine_set_pad_press(int raw);
