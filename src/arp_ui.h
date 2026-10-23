/*
 * The arpeggiator's settings screen (Shift + Solo; Solo alone switches it on and off, on every screen). The editor
 * routes the buttons and encoders here while it is open. The engine is arp.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "synth.h"

void arp_ui_init(void);                    /* at boot: defaults, the saved settings */
void arp_ui_draw(void);                    /* everything (the displays are cleared first) */
void arp_ui_encoder(int i, int delta);
void arp_ui_main_encoder(int steps);       /* tempo */
void arp_ui_toggle(void);                  /* Solo */
void arp_ui_tick(int64_t now_us, bool open);  /* saves the settings a moment after the last change; moves the picture */
const synth_tip_t *arp_ui_tips(int *count);   /* the tutorial (Shift + Control) */
