/*
 * What the JV-880's synth entries share: its SysEx messages (Roland model
 * ID 46H, manual p. 222) and its 4-byte addresses.
 *
 * An address is kept as one number: the four 7-bit address bytes packed
 * together (00 08 28 00 = 0x00020400 here), so adding an offset is the
 * unit's own 7-bit arithmetic. Write base addresses as their bytes with
 * JV880_ADDR(), never as a hex number of the bytes' values.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JV880_MODEL_ID 0x46
#define JV880_ADDR(a, b, c, d) ((uint32_t)(a) << 21 | (uint32_t)(b) << 14 | (uint32_t)(c) << 7 | (uint32_t)(d))

/* Start addresses (manual p. 223) */
#define JV880_SYSTEM_COMMON JV880_ADDR(0x00, 0x00, 0x00, 0x00)
#define JV880_TEMP_PATCH JV880_ADDR(0x00, 0x08, 0x20, 0x00)  /* Patch Mode Temporary Patch */

/* Patch (manual p. 227): common at +00 00, tone n (0-3) at +08 00 + n * 01 00 */
#define JV880_PATCH_COMMON_LEN 0x22
#define JV880_PATCH_TONE_LEN 0x74
#define JV880_PATCH_LEN (JV880_PATCH_COMMON_LEN + 4 * JV880_PATCH_TONE_LEN)
#define JV880_TONE_ADDR(patch_base, n) ((patch_base) + JV880_ADDR(0, 0, 8 + (n), 0))

/* DT1 (data set) into out (needs 13 + len bytes); returns the length.
 * dev is the device ID: unit number - 1 */
size_t jv880_dt1(int dev, uint32_t addr, const uint8_t *data, size_t len, uint8_t *out);
/* RQ1 (data request) for `size` bytes (needs 17 bytes); the reply is a DT1 */
size_t jv880_rq1(int dev, uint32_t addr, uint32_t size, uint8_t *out);
/* A DT1 from the unit: its address and data (pointer into d), or false if
 * it isn't one, isn't from this device or its checksum is wrong */
int jv880_parse_dt1(const uint8_t *d, size_t len, int dev, uint32_t *addr, const uint8_t **data);

/* ---- expansion boards and waveforms ----
 * The CARD setting (0 = none, 1-19 = SR-JV80-nn, 20-22 = boards 97-99) names
 * the board in the unit; its wave names come from /lfs/jv880/waves. */
#define JV880_WAVE_NAME_LEN 16
extern const char *const jv880_card_names[23];  /* shortened, 12 characters */
int jv880_card_number(int setting);             /* setting value -> board number */
/* Wave group 0 = INT, 1 = EXP (the board's), 2 = PCM; card = board number */
int jv880_wave_count(int group, int card);
bool jv880_wave_name(int group, int number, int card, char *buf, size_t len);
bool jv880_wave_exists(int group, int number, int card);
int jv880_wave_nearest(int group, int number, int card);
/* A wave column's picture: the number big (num), the name below when known */
void jv880_draw_wave(int disp, int x, int y, int w, const char *num, int group, int number, int card);

/* A code big and its name below in a column (the name shrinks or splits to fit) */
void jv880_draw_code_name(int disp, int x, int y, int w, const char *code, const char *name);

/* An envelope over both displays: points at levels lv[] (0-100), reached
 * after times tm[] (0-127, wider the slower); a held sustain segment after
 * point sus_at; centre line for the pitch envelope. Text below at
 * JV880_EG_TEXT_CAP_Y. */
#define JV880_EG_TEXT_CAP_Y 55
void jv880_draw_env(int disp, const int *lv, const int *tm, int n, int sus_at, bool centred);
/* The same, dotted (an earlier shape left to fade: see the SR's trail) and/or without clearing the band first */
void jv880_draw_env_ex(int disp, const int *lv, const int *tm, int n, int sus_at, bool centred, bool dotted,
                       bool clear);
/* An envelope column's "T1 50" text at the bottom */
void jv880_draw_eg_text(int disp, int x, const char *name, const char *value);
