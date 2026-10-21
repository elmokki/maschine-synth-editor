/*
 * Wavestation SR wave sequences. The bank dump (request 0C, reply 54, 17576 bytes) is 8784 bytes as nibbles:
 * 32 sequences x 16, 501 steps x 16, 32 names x 8 (all big-endian words; layout from Edisyn's
 * KorgWavestationSequence.java). A sequence's steps are a linked list (sequence.link = first, step.fLink = next,
 * 0 = end). ws_seq_extract lifts one sequence out as a library item (what /lfs/wsseq/NNN name holds):
 *
 *   0  name, 16 bytes (the dump's 8 characters + spaces)
 *  16  length (steps, 0-255)
 *  17  loop start, loop end (1-based), loop count (0 = off, 127 = infinite), back and forth (0/1), start step
 *      (as the dump has it: Edisyn never reads it, so what it means is not known), mod source, mod amount
 *      (signed), dyno mod (2), start time (2), time (2) - the last three kept as they were
 *  30  steps, 10 bytes each: wave (2, big-endian: index into the 486 PCM waves, the dump's wave number - 32, expanded
 *      PCM and the high byte decoded as Edisyn does), coarse (signed semitones), fine (signed), crossfade (2),
 *      duration (2), level, flags (bit 0: card wave - the dump does not say, the editor only)
 */
#pragma once
#include <stdint.h>

#define WSSEQ_HEADER 30
#define WSSEQ_STEP_LEN 10
#define WSSEQ_MAX_STEPS 255
#define WSSEQ_MAX_LEN (WSSEQ_HEADER + WSSEQ_MAX_STEPS * WSSEQ_STEP_LEN)

/* Sequence n (0-31) of a bank dump (nibbles from the byte after the header) into raw; returns its length, or 0
 * when the slot is empty (no name and no steps) */
int ws_seq_extract(const uint8_t *nib, int n, uint8_t *raw);

/* Sequence n (0-31) of a bank dump replaced by the item raw (the layout above): its steps go into free step slots,
 * the links, loop and start are made as the unit's own sequences have them. NULL = done, else why not. */
const char *ws_seq_put(uint8_t *nib, int n, const uint8_t *raw);
