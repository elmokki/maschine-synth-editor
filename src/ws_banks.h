/*
 * What the Wavestation SR's synth entries share: its bank numbers, and the jobs that read whole banks from the
 * unit (IMPORT into the library) or put one item into a bank and send the bank back (REC > TO SR).
 *
 * The SR has no single-item write. A bank's patches (or performances) are one message, F0 42 3n 28 <4C|4D> bank
 * <items as nibbles> sum F7 (29828 bytes for the 35 patches, 18108 for the 50 performances), asked for with
 * 1C / 1D. Storing reads the bank, replaces the item and sends the whole bank back, paced for the DIN cable; the
 * unit's memory protect (GLOBAL page) must be off, (a Yes on the SR is not needed: tested)
 * (Edisyn says a Yes is needed; tested here: it is not). The bank as it was is kept in /lfs/ws/backup first. A job takes the whole screen
 * (synth_t.busy) and can't be cancelled. Library numbers: bank * count + item + 1.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bank numbers: Edisyn's order (RAM 1-3, ROM 4-11, Card) <-> the SR's own (parameter messages, dump headers), and the
 * order of the wave bank in a patch's data (low 2 bits + Bank_Exp bits) */
extern const uint8_t ws_to_ws_bank[12];
extern const uint8_t ws_from_ws_bank[12];
extern const uint8_t ws_to_ws_exp_bank[12];
extern const uint8_t ws_from_ws_exp_bank[12];
extern const char *const ws_bank_names[12];  /* "RAM 1" ... "CARD" */
extern const char *const ws_bank_tags[12];   /* "R1" ... "ROM10", "CD" */

typedef struct {
    const char *item;     /* "patch", "performance" */
    const char *dir;      /* library directory (full path) */
    const char *fallback; /* file name of an item with no name */
    const char *backup;   /* /lfs/ws/backup/<backup>NN */
    uint8_t request, reply;
    int count;            /* items in a bank: 35, 50 */
    int item_len;         /* raw bytes of an item: 426, 181 */
    /* Kinds whose dump is not count x item_len nibbles (wave sequences): the dump's total length, and the item n
     * lifted out of the dump (nibbles from the byte after the 6 byte header) into raw; returns the raw length, 0 if
     * the item is empty. Such a kind can only be imported, not stored. */
    int dump_len;
    int (*extract)(const uint8_t *nib, int n, uint8_t *raw);
} ws_kind_t;

/* The two kinds the SR's drivers use: patches (35 a bank, 426 bytes) and performances (50, 181) */
extern const ws_kind_t ws_kind_patch, ws_kind_perf, ws_kind_seq;

/* Reads the banks in mask (bit n = bank n, Edisyn's order) into the library */
void ws_banks_import(const ws_kind_t *kind, uint16_t mask, int channel);
/* Puts raw (item_len bytes) into bank / num of the unit: reads the bank, replaces, sends it back */
void ws_banks_store(const ws_kind_t *kind, int bank, int num, const uint8_t *raw, int channel);
bool ws_banks_busy(void);
void ws_banks_tick(int64_t now);
void ws_banks_draw(int disp);
/* Offer every SysEx from the unit: true if it was the job's (a bank dump, or a status message 21-24) */
bool ws_banks_sysex(const uint8_t *d, size_t len);
/* The library's name of an item (after an import), "" when unknown */
const char *ws_banks_lib_name(const ws_kind_t *kind, int bank, int num);

/* A wave's name (names kept in memory, from /lfs/ws/waves and /lfs/ws/wavesq): bank 0-11 in Edisyn's order, num =
 * the SR's wave number (0-31 the bank's wave sequences, 32+ the PCM waves; a card has its own: empty). "" if unknown */
#define WS_WAVE_NAME_LEN 16
void ws_wave_name(int bank, int num, char *buf, size_t len);
