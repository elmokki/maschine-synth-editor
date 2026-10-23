/*
 * Arpeggiator (Solo button): the notes of the held pads are played one after another instead of together, on the
 * pad channel. It is clocked by the sequencer's 96 ppq timer (sequencer.c): in time with the sequencer while that
 * plays (a note starts on the next grid step of the RATE), else on a clock of its own that runs only while the
 * arpeggiator has something to play. The pad handler (editor.c) hands the pads over here; the screen is arp_ui.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { ARP_UP, ARP_DOWN, ARP_UPDN, ARP_DNUP, ARP_ORDER, ARP_RANDOM, ARP_CHORD, ARP_MODES } arp_mode_t;
#define ARP_RATES 6

typedef struct {
    bool on;
    uint8_t mode;       /* arp_mode_t */
    uint8_t rate;       /* index of arp_rate_names / arp_rate_ticks */
    uint8_t gate;       /* percent of a step, 5-100 */
    uint8_t octaves;    /* 1-4 */
    uint8_t swing;      /* 50-75: percent of a pair of steps the first one takes */
    uint8_t vel;        /* 0 = the pad's own, else 1-127 */
    bool latch;         /* the notes stay after the pads are released, until a new group is played */
    bool retrig;        /* a new pad starts the pattern over */
} arp_cfg_t;

extern arp_cfg_t arp_cfg;
extern const char *const arp_mode_names[ARP_MODES];
extern const char *const arp_rate_names[ARP_RATES];

void arp_defaults(void);
/* Call after changing arp_cfg: releases latched notes when latch went off, silences when it went off */
void arp_changed(void);
/* A pad went down with these notes (the chord), on this channel: true = the arpeggiator took them (the caller
 * sends no note-on); false = not on, play as usual */
bool arp_pad_down(int pad, const uint8_t *notes, int count, int channel, int vel);
void arp_pad_up(int pad);
/* Notes from an external source (DIN IN), on the pad channel: like pads. Down: true = taken; up: true = it was one
 * the arpeggiator holds (else the note-off belongs to the synth) */
bool arp_ext_down(int note, int channel, int vel);
bool arp_ext_up(int note);
void arp_panic(void);            /* forget everything, end the notes */
void arp_pause(bool paused);     /* a SysEx job runs: no notes */
/* For the screen: the notes of one pass of the pattern in playing order (RANDOM: the held notes by pitch; CHORD:
 * the lowest note in each octave), up to max; returns how many. Empty when nothing is held */
int arp_cycle(uint8_t *notes, int max);
int arp_held(uint8_t *notes, int max);   /* the notes held, by pitch (ORDER: by press) */
int arp_step(void);              /* the position in that pass now, -1 = not playing */

/* The clock side (sequencer.c's task) */
void arp_tick(uint32_t tick, bool from_sequencer);
void arp_service(void);          /* once per wake of the task: flushes */
bool arp_active(void);           /* needs the clock: pads held or latched, a note sounding */
void arp_silence(void);          /* ends the sounding notes */
