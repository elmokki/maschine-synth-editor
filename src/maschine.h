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

/* Mono LED of the button with this report 0x01 input bit (see the button
 * table in docs/PROTOCOL.md), brightness 0-127. Every button except the
 * Group buttons (RGB, above) has one. */
void maschine_set_button_led(int bit, uint8_t value);
