/*
 * The editor UI on the Maschine's displays, for whichever synth driver is
 * active (see synth.h; MiniMax for now).
 *
 * Pages of 8 parameters, one per endless encoder, plus up to 8 switches on
 * the display buttons; Group buttons pick sections. The main encoder and
 * the top arrow buttons change page; the arrows next to Enter (or the main
 * encoder, while Browse is toggled on) browse the patch library on
 * LittleFS and Enter loads the selected patch. Rec stores/renames/deletes
 * patches, Control opens the synth's setup (MIDI channel, pad note, ...).
 *
 * Only draws into the display framebuffers and queues MIDI; the caller
 * (main.c) pushes dirty display chunks out afterwards. Not thread-safe:
 * call everything from the USB client event loop.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void editor_init(void);
void editor_button(int bit, bool down);    /* report 0x01 button bit */
void editor_encoder(int i, int delta);     /* 0-7, raw counter units */
void editor_main_encoder(int steps);       /* +-1 per detent */
void editor_pad(int pad, bool down, int raw);  /* raw pressure 0-4095 */
void editor_pad_pressure(int pad, int raw);  /* while a pad is held: aftertouch */
void editor_tick(int64_t now_us);          /* patch browse timeout */
void editor_redraw(void);                  /* e.g. after a reconnect */
void editor_connected(void);               /* Maschine (and DIN MIDI) ready */
