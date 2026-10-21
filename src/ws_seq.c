#include <stdbool.h>
#include <string.h>
#include "ws_seq.h"

#define SEQ_OFF 0
#define STEP_OFF (32 * 16)
#define NAME_OFF (STEP_OFF + 501 * 16)

static inline uint8_t db(const uint8_t *nib, int i) { return nib[2 * i] | nib[2 * i + 1] << 4; }
static inline int dw(const uint8_t *nib, int i) { return db(nib, i) << 8 | db(nib, i + 1); }

int ws_seq_extract(const uint8_t *nib, int n, uint8_t *raw)
{
    const int s = SEQ_OFF + n * 16;
    bool blank = true;
    memset(raw, ' ', 16);
    for (int i = 0; i < 8; i++) {
        uint8_t c = db(nib, NAME_OFF + n * 8 + i);
        raw[i] = c >= 32 && c < 127 ? c : ' ';
        blank = blank && raw[i] == ' ';
    }
    int step = dw(nib, s);
    int len = 0;
    uint8_t *dst = raw + WSSEQ_HEADER;
    while (step != 0 && step < 501 && len < WSSEQ_MAX_STEPS) {
        const int t = STEP_OFF + step * 16;
        int wave = db(nib, t + 7) + (db(nib, t + 6) == 0x81 ? 256 : 0);  /* Edisyn: high byte 129 = +256 */
        int coarse = (int8_t)db(nib, t + 8);
        if (coarse >= 48) {  /* expanded PCM: coarse is 72 too high and the wave 365 too low */
            coarse -= 72;
            wave += 365;
        }
        wave -= 32;  /* the first 32 numbers are wave sequences */
        dst[0] = wave >> 8 & 0xFF;
        dst[1] = wave & 0xFF;
        dst[2] = (uint8_t)coarse;
        dst[3] = db(nib, t + 9);   /* fine */
        dst[4] = db(nib, t + 10);  /* crossfade */
        dst[5] = db(nib, t + 11);
        dst[6] = db(nib, t + 12);  /* duration */
        dst[7] = db(nib, t + 13);
        dst[8] = db(nib, t + 14);  /* level */
        dst[9] = 0;                /* flags */
        dst += WSSEQ_STEP_LEN;
        len++;
        step = dw(nib, t);  /* fLink */
    }
    if (len == 0 && blank) {
        return 0;
    }
    raw[16] = len;
    raw[17] = db(nib, s + 4) + 1;  /* loop start: 1 is 0 in the dump */
    raw[18] = db(nib, s + 5) + 1;
    raw[19] = db(nib, s + 6) & 127;
    raw[20] = db(nib, s + 6) >> 7;
    raw[21] = db(nib, s + 7) + 1;  /* start step: 0-based in the dump */
    raw[22] = db(nib, s + 8) & 127;
    raw[23] = db(nib, s + 9);
    for (int i = 0; i < 6; i++) {
        raw[24 + i] = db(nib, s + 10 + i);  /* dyno mod, start time, time */
    }
    return WSSEQ_HEADER + len * WSSEQ_STEP_LEN;
}

/* ---- writing one sequence into a bank dump ----
 * What the unit's own sequences show (read 2026-10-21 from RAM 1 and 2): step links fLink / bLink with 0 for none
 * and steps 0 and 500 untouched sentinels; a step's lLink is -2 (65534) at the loop's first step, the loop's first
 * step's number at its last step, -1 (65535) elsewhere (the last step wins if both are one step); sLink = the start
 * step's number; the sequence's time = the durations' sum + the last step's crossfade / 2; start time = the durations
 * before the start step (a guess: always 0 with start 1); mod index of a step 127 and dyno mod 0 when no modulation
 * is set; mod source byte = 0x80 | source. */

static inline void sdb(uint8_t *nib, int i, int v)
{
    nib[2 * i] = v & 0x0F;
    nib[2 * i + 1] = v >> 4 & 0x0F;
}
static inline void sdw(uint8_t *nib, int i, int v)
{
    sdb(nib, i, v >> 8 & 0xFF);
    sdb(nib, i + 1, v & 0xFF);
}

const char *ws_seq_put(uint8_t *nib, int n, const uint8_t *raw)
{
    static bool used[501];
    int ids[WSSEQ_MAX_STEPS];
    const int len = raw[16];
    if (len < 1 || len > WSSEQ_MAX_STEPS) {
        return "THE SEQUENCE HAS NO STEPS";
    }
    memset(used, 0, sizeof(used));
    used[0] = used[500] = true;
    for (int q = 0; q < 32; q++) {
        if (q == n) {
            continue;  /* its steps are free again */
        }
        int step = dw(nib, SEQ_OFF + q * 16);
        for (int guard = 0; step > 0 && step < 501 && guard < 501; guard++) {
            used[step] = true;
            step = dw(nib, STEP_OFF + step * 16);
        }
    }
    int k = 0;
    for (int i = 1; i < 500 && k < len; i++) {
        if (!used[i]) {
            ids[k++] = i;
        }
    }
    if (k < len) {
        return "NO FREE STEPS IN THE BANK";
    }
    for (int j = 0; j < len; j++) {
        if (raw[WSSEQ_HEADER + j * WSSEQ_STEP_LEN + 9] & 1) {
            return "CARD WAVES CAN'T BE WRITTEN";
        }
    }
    int ls = raw[17] - 1, le = raw[18] - 1, start = raw[21] - 1;
    ls = ls < 0 ? 0 : ls > len - 1 ? len - 1 : ls;
    le = le < ls ? ls : le > len - 1 ? len - 1 : le;
    start = start < 0 ? 0 : start > len - 1 ? len - 1 : start;
    long time = 0, start_time = 0;
    int last_xfade = 0;
    for (int j = 0; j < len; j++) {
        const uint8_t *st = raw + WSSEQ_HEADER + j * WSSEQ_STEP_LEN;
        const int t = STEP_OFF + ids[j] * 16;
        int wave = (st[0] << 8 | st[1]) + 32, coarse = (int8_t)st[2];
        int hi = 0x80;
        if (wave >= 365) {  /* expanded PCM: the unit's own coding */
            wave -= 365;
            coarse += 72;
        } else if (wave >= 256) {
            wave -= 256;
            hi = 0x81;
        }
        int dur = st[6] << 8 | st[7], xf = st[4] << 8 | st[5];
        sdw(nib, t, j + 1 < len ? ids[j + 1] : 0);
        sdw(nib, t + 2, j > 0 ? ids[j - 1] : 0);
        sdw(nib, t + 4, j == le ? ids[ls] : j == ls ? 65534 : 65535);
        sdb(nib, t + 6, hi);
        sdb(nib, t + 7, wave);
        sdb(nib, t + 8, coarse & 0xFF);
        sdb(nib, t + 9, st[3]);
        sdw(nib, t + 10, xf);
        sdw(nib, t + 12, dur);
        sdb(nib, t + 14, st[8]);
        sdb(nib, t + 15, 127);
        time += dur;
        if (j < start) {
            start_time += dur;
        }
        last_xfade = xf;
    }
    const int s = SEQ_OFF + n * 16;
    sdw(nib, s, ids[0]);
    sdw(nib, s + 2, ids[start]);
    sdb(nib, s + 4, ls);
    sdb(nib, s + 5, le);
    sdb(nib, s + 6, (raw[19] & 127) | (raw[20] & 1) << 7);
    sdb(nib, s + 7, start);
    sdb(nib, s + 8, 0x80 | (raw[22] & 127));
    sdb(nib, s + 9, 0);  /* modulation amount: not written (its per-step indexes are not understood) */
    sdw(nib, s + 10, 0);
    sdw(nib, s + 12, (int)start_time);
    sdw(nib, s + 14, (int)(time + last_xfade / 2));
    for (int i = 0; i < 8; i++) {
        sdb(nib, NAME_OFF + n * 8 + i, raw[i] >= 32 && raw[i] < 127 ? raw[i] : ' ');
    }
    return NULL;
}
