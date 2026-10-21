/*
 * The sequencer's screen (Scene button): the editor routes the buttons, pads and encoders here while it is open.
 * Group A-H pick the track (each its own colour, the picked one bright), the pads are the 16 steps of the shown
 * page, Step arrows turn the page, knobs edit the picked step and the track. Rec = pads write (on) or only pick
 * (off); Pattern = the 16 pattern slots on the pads (kept on LittleFS, written a moment after each edit);
 * display button 1 = TIE, 5 = MUTE; Control = the sequencer's settings (MIDI IN channel). The engine is sequencer.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "synth.h"

void seq_ui_init(void);                    /* at boot: settings, pattern slots, the pattern in use */
void seq_ui_enter(void);                   /* the screen was just opened (the displays are cleared) */
void seq_ui_draw(void);                    /* everything */
bool seq_ui_button(int bit, bool down);    /* true = used here */
void seq_ui_pad(int pad, bool down, int raw);
void seq_ui_encoder(int i, int delta);
void seq_ui_main_encoder(int steps);       /* tempo */
void seq_ui_tick(int64_t now_us);          /* playhead, redraws */
void seq_ui_midi_note(int channel, int note, int velocity);  /* DIN IN, velocity 0 = off */
void seq_ui_group_leds(void);
void seq_ui_button_leds(uint8_t control_led);  /* every button once; control_led = Control's value while Shift is held (0 = as usual) */
const synth_tip_t *seq_ui_tips(int *count);    /* the tutorial (Shift + Control) */
void seq_ui_leave(void);                   /* gives the pads back to the editor: it sets their LEDs */
