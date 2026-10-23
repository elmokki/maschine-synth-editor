/*
 * Step sequencer engine: 8 tracks, up to 128 steps each (16th notes), one MIDI channel per track. It runs in its
 * own task on core 1, clocked by a hardware timer (the FreeRTOS tick is 10 ms: too coarse), 96 ticks per quarter.
 * The UI (editor.c, Scene) edits the pattern; the engine only reads it, so single-byte races are harmless.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SQ_TRACKS 8
#define SQ_STEPS 128
#define SQ_POLY 1            /* notes per step (chords later) */
#define SQ_PPQ 96            /* ticks per quarter note */
#define SQ_STEP_TICKS 24     /* a 16th note */

typedef struct {
    uint8_t note[SQ_POLY];
    uint8_t vel;             /* 0 = empty step */
    uint8_t gate;            /* percent of a step (of a ratchet hit), 5-100 */
    uint8_t ratchet;         /* 1-8 evenly spaced hits in the step */
    bool tie;                /* continues the previous note: no new note-on, a different note is played legato
                              * (new note on before the old one's off); the note before it holds for the whole
                              * step and the chain ends with the last tied step's own gate */
} sq_step_t;

typedef struct {
    sq_step_t steps[SQ_STEPS];
    uint8_t length;          /* 1-128 */
    uint8_t channel;         /* 0-15 */
    int8_t transpose;        /* semitones */
    bool mute;
} sq_track_t;

typedef struct {
    sq_track_t track[SQ_TRACKS];
    uint16_t bpm;            /* 20-300 */
} sq_pattern_t;

extern sq_pattern_t sq_pattern;
extern bool sq_clock_out;   /* MIDI clock (24 ppq), start and stop to DIN OUT while the sequencer plays */

void sq_pattern_clear(sq_pattern_t *p);   /* an empty pattern: 120 BPM, 16 steps, track n on channel n */
/* Makes this the playing pattern: at once when stopped, else at the next pass of the longest track (the
 * pattern is copied, a later change to p does nothing) */
void sq_load(const sq_pattern_t *p);
bool sq_pending(void);                    /* a loaded pattern is still waiting for its turn */
void sq_init(void);          /* starts the task and the timer (once) */
void sq_play(bool on);       /* stopping sends the notes off */
bool sq_playing(void);
void sq_restart(void);       /* to the first step */
void sq_set_bpm(int bpm);
/* The step each track is playing next/now, tracks of different lengths loop on their own (-1 = stopped) */
int sq_position(int track);
void sq_clock_wake(void);  /* the arpeggiator has something to play: starts the clock if it is stopped */
