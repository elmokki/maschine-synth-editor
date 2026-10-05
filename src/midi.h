/*
 * DIN MIDI through the Maschine Mk2's standard USB-MIDI class interface
 * (IF1, bulk endpoints - see docs/PROTOCOL.md). Messages sent here come out
 * of the Maschine's DIN OUT jack; whatever arrives on its DIN IN jack is
 * logged.
 *
 * Like main.c's HID code, everything here runs in the USB host client
 * event loop, so it isn't thread-safe and must be called from there.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "usb/usb_host.h"

/* Claims IF1 and starts listening. Returns false if the interface or its
 * endpoints aren't there or can't be claimed. */
bool midi_attach(usb_host_client_handle_t client, usb_device_handle_t dev,
                 const usb_config_desc_t *cfg);
void midi_detach(void);

/* Queues one channel voice message (status 0x80-0xEF) for DIN OUT. */
void midi_send(uint8_t status, uint8_t data1, uint8_t data2);

/* Queues a whole System Exclusive message (F0 ... F7 included) for DIN OUT;
 * false if it doesn't fit the queue right now (nothing is sent then). */
bool midi_send_sysex(const uint8_t *data, size_t len);

/* What arrives on DIN IN goes back out of DIN OUT: not at all, as is, or
 * with channel messages moved to `channel` (the selected synth's). SysEx
 * goes out once complete, so it never mixes with the editor's own. */
typedef enum { MIDI_THRU_OFF, MIDI_THRU_ALL, MIDI_THRU_TO_CHANNEL } midi_thru_t;
void midi_set_thru(midi_thru_t mode, int channel);

/* Called with every complete System Exclusive message arriving on DIN IN
 * (F0 ... F7), e.g. a synth's reply to a data request; NULL = none */
typedef void (*midi_sysex_handler_t)(const uint8_t *data, size_t len);
void midi_set_sysex_handler(midi_sysex_handler_t fn);
