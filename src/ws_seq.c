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
    raw[21] = db(nib, s + 7);
    raw[22] = db(nib, s + 8) & 127;
    raw[23] = db(nib, s + 9);
    for (int i = 0; i < 6; i++) {
        raw[24 + i] = db(nib, s + 10 + i);  /* dyno mod, start time, time */
    }
    return WSSEQ_HEADER + len * WSSEQ_STEP_LEN;
}
