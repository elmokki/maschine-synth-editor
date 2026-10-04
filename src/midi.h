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
#include <stdint.h>
#include "usb/usb_host.h"

/* Claims IF1 and starts listening. Returns false if the interface or its
 * endpoints aren't there or can't be claimed. */
bool midi_attach(usb_host_client_handle_t client, usb_device_handle_t dev,
                 const usb_config_desc_t *cfg);
void midi_detach(void);

/* Queues one channel voice message (status 0x80-0xEF) for DIN OUT. */
void midi_send(uint8_t status, uint8_t data1, uint8_t data2);
