/*
 * What the D-110's three synth entries (TONE, PATCH, RHYTHM: one unit)
 * share: its volume, sent as Main Volume (CC7) on every part's channel -
 * the unit has no master volume over MIDI.
 */
#pragma once
#include <stdint.h>

int d110_volume_get(void);
void d110_volume_set(int value);          /* sends it and remembers it */
/* The channels the unit's parts listen on (bit n = channel n+1), as far as
 * the drivers know; default the D-110's own: 2-10 */
void d110_set_channels(uint16_t mask);
