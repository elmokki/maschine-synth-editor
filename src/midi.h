/*
 * DIN MIDI through the Maschine Mk2's standard USB-MIDI class interface
 * (IF1, bulk endpoints - see docs/PROTOCOL.md). Messages sent here come out
 * of the Maschine's DIN OUT jack; whatever arrives on its DIN IN jack is
 * logged.
 *
 * The USB side runs in the USB host client event loop. The send functions
 * (midi_send, midi_send_sysex, midi_send_sysex_part) may also be called from
 * another task, e.g. the sequencer's: the OUT queue is guarded by a mutex.
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

/* Queues one System Real-Time byte (0xF8 clock, 0xFA start, 0xFC stop ...) for DIN OUT */
void midi_send_realtime(uint8_t status);

/* Queues a whole System Exclusive message (F0 ... F7 included) for DIN OUT;
 * false if it doesn't fit the queue right now (nothing is sent then). */
bool midi_send_sysex(const uint8_t *data, size_t len);

/* The same for a message too long to queue: sent in parts, the last one ending with F7, the others a multiple of
 * 3 bytes; false = no room right now (nothing sent). The caller paces it (the DIN cable: ~3 KB/s). */
bool midi_send_sysex_part(const uint8_t *data, size_t len, bool last);

/* What arrives on DIN IN goes back out of DIN OUT: not at all, as is, or
 * with channel messages moved to `channel` (the selected synth's). SysEx
 * goes out once complete, so it never mixes with the editor's own. */
typedef enum { MIDI_THRU_OFF, MIDI_THRU_ALL, MIDI_THRU_TO_CHANNEL } midi_thru_t;
void midi_set_thru(midi_thru_t mode, int channel);

/* Called with every complete System Exclusive message arriving on DIN IN
 * (F0 ... F7), e.g. a synth's reply to a data request; NULL = none */
typedef void (*midi_sysex_handler_t)(const uint8_t *data, size_t len);
void midi_set_sysex_handler(midi_sysex_handler_t fn);

/* Called with every note arriving on DIN IN: velocity 0 = note off. True = taken: it is not sent through */
typedef bool (*midi_note_handler_t)(int channel, int note, int velocity);
void midi_set_note_handler(midi_note_handler_t fn);
