/*
 * The arpeggiator's settings screen (Shift + Solo; Solo alone switches it on and off, on every screen). The editor
 * routes the buttons and encoders here while it is open. The engine is arp.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void arp_ui_init(void);                    /* at boot: defaults, the saved settings */
void arp_ui_draw(void);                    /* everything (the displays are cleared first) */
bool arp_ui_button(int bit, bool down);    /* display buttons: LATCH, RETRIG; true = used */
void arp_ui_encoder(int i, int delta);
void arp_ui_main_encoder(int steps);       /* tempo */
void arp_ui_toggle(void);                  /* Solo */
void arp_ui_tick(int64_t now_us);          /* saves the settings a moment after the last change */
