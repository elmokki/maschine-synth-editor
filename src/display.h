/*
 * The Maschine Mk2's two 256x64 1bpp displays: framebuffers, drawing, and
 * chunked output reports (0xE0 / 0xE1, see docs/PROTOCOL.md).
 *
 * Drawing only touches the framebuffer and marks the affected 8-row chunks
 * dirty; display_next_packet() then hands out one 265-byte HID report per
 * dirty chunk, so only what changed goes over USB.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fonts.h"

#define DISPLAY_COUNT 2
#define DISPLAY_WIDTH 256
#define DISPLAY_HEIGHT 64
#define DISPLAY_USABLE_WIDTH 255  /* column x=255 is cropped by the panel */
#define DISPLAY_PACKET_SIZE 265   /* 9-byte header + 8 rows x 32 bytes */

void display_clear(int disp);
void display_pixel(int disp, int x, int y, bool on);
void display_fill_rect(int disp, int x, int y, int w, int h, bool on);
void display_rect(int disp, int x, int y, int w, int h, bool on);

/* Draws text with its top-left at (x, y); returns the x after the last glyph. */
int display_text(int disp, int x, int y, const font_t *font, const char *s, bool on);
int display_text_width(const font_t *font, const char *s);

/* Marks every chunk of both displays dirty, e.g. after a reconnect. */
void display_invalidate_all(void);

/* Fills out[DISPLAY_PACKET_SIZE] with the next dirty chunk and clears its
 * dirty bit. Returns false when nothing is dirty. */
bool display_next_packet(uint8_t *out);
