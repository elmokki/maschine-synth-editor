/*
 * Phone-keypad style text entry on the 4x4 pads, for naming patches.
 *
 *   [1 .-'&!] [2 ABC] [3 DEF]  [DEL]
 *   [4 GHI]   [5 JKL] [6 MNO]  [Aa]
 *   [7 PQRS]  [8 TUV] [9 WXYZ] [<]
 *   [()+/#]   [0 spc] [ ]      [>]
 *
 * Tapping the same pad again within a second cycles through its
 * characters; another pad or a pause accepts the character. Draws the text
 * on the left display (below the header/soft-key rows the caller owns) and
 * the keypad legend on the right display, and colours the pads to match.
 * Same threading rule as the rest of the UI: call from the USB client loop.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TEXTENTRY_MAX_LEN 20

void textentry_start(const char *initial);
void textentry_stop(void);
bool textentry_active(void);
void textentry_pad(int pad, int64_t now_us);
void textentry_tick(int64_t now_us);
const char *textentry_text(void);

/* Draws the text (left display, rows from y0 down) and the keypad legend
 * (whole right display) */
void textentry_draw(int y0);
