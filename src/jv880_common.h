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
