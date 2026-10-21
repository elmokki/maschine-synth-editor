#include "ws_fx.h"
#include <stdio.h>
#include <string.h>

extern const char *const ws_fx_rates[], *const ws_fx_splits[], *const ws_fx_freqs[], *const ws_fx_shapes[];

#define RATES 100
#define SHAPES 42
#define SPLITS 19
#define FREQS 30

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int unit_offset(int unit) { return unit == 1 ? 5 : 13; }

int ws_fx_type_get(const uint8_t *fx, int unit)
{
    int t = (fx[unit - 1] & 63) - 2;
    int ext = fx[unit_offset(unit)] & 15;
    if ((fx[1] & 0x80) && t == 0 && ext > 0) {
        t = 47 + ext;
    }
    return t < 0 || t >= WS_FX_TYPES ? 0 : t;
}

void ws_fx_type_set(uint8_t *fx, int unit, int type)
{
    type = clampi(type, 0, WS_FX_TYPES - 1);
    uint8_t *u = fx + unit_offset(unit);
    memcpy(u, ws_fx_type_preset[type], 8);
    /* The extended effects (48-55) are number 1-8 in the unit's first nibble, and the type byte says "none" */
    if (type >= 48) {
        u[0] = (u[0] & 0xF0) | (type - 47);
        fx[unit - 1] = (fx[unit - 1] & ~63) | 2;
        fx[1] |= 0x80;
    } else {
        fx[unit - 1] = (fx[unit - 1] & ~63) | (type + 2);
    }
}

int ws_fx_get(const uint8_t *unit_bytes, const ws_fx_param_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < p->npieces; i++) {
        const ws_fx_piece_t *pc = &p->pieces[i];
        int len = pc->len < 0 ? -pc->len : pc->len;
        uint32_t raw = (unit_bytes[pc->byte] >> pc->bit) & ((1u << len) - 1);
        if (pc->len < 0 && (raw >> (len - 1) & 1)) {
            raw |= ~0u << len;  /* sign extension */
        }
        v |= raw << pc->pbit;
    }
    return clampi((int32_t)v, p->min, p->max);
}

void ws_fx_put(uint8_t *unit_bytes, const ws_fx_param_t *p, int value)
{
    uint32_t v = (uint32_t)clampi(value, p->min, p->max);
    for (int i = 0; i < p->npieces; i++) {
        const ws_fx_piece_t *pc = &p->pieces[i];
        int len = pc->len < 0 ? -pc->len : pc->len;
        uint32_t mask = (1u << len) - 1;
        uint32_t raw = (v >> pc->pbit) & mask;
        unit_bytes[pc->byte] = (unit_bytes[pc->byte] & ~(mask << pc->bit)) | (raw << pc->bit);
    }
}

void ws_fx_format(const ws_fx_param_t *p, int v, char *buf, size_t len)
{
    switch (p->fmt) {
    case WS_FX_MIX100:
        v == 0 ? snprintf(buf, len, "DRY") : v == 100 ? snprintf(buf, len, "WET") : snprintf(buf, len, "%d/%d", 100 - v, v);
        break;
    case WS_FX_MIX11:
        v <= 1 ? snprintf(buf, len, "DRY") : v >= 11 ? snprintf(buf, len, "WET") : snprintf(buf, len, "%d/%d", 11 - v, v - 1);
        break;
    case WS_FX_PM10:
        if (v == 0) {
            snprintf(buf, len, "DRY");
        } else if (v == 10) {
            snprintf(buf, len, "WET");
        } else if (v == -10) {
            snprintf(buf, len, "-WET");
        } else if (v > 0) {
            snprintf(buf, len, "%d/%d", 10 - v, v);
        } else {
            snprintf(buf, len, "-%d/%d", 10 + v, -v);
        }
        break;
    case WS_FX_RATE_FIXED:
        if (v == 0) {
            snprintf(buf, len, "FIXED");
            break;
        }
        /* fall through */
    case WS_FX_RATE:
        snprintf(buf, len, "%s Hz", ws_fx_rates[clampi(v, 1, RATES) - 1]);
        break;
    case WS_FX_SHAPE:
        snprintf(buf, len, "%s", ws_fx_shapes[clampi(v + 21, 0, SHAPES - 1)]);
        break;
    case WS_FX_SPLIT:
        snprintf(buf, len, "%s", ws_fx_splits[clampi(v, 0, SPLITS - 1)]);
        break;
    case WS_FX_FREQ:
        snprintf(buf, len, "%s", ws_fx_freqs[clampi(v, 0, FREQS - 1)]);
        break;
    case WS_FX_MANUAL:
        v == 0 ? snprintf(buf, len, "MANUAL") : snprintf(buf, len, "%d", v);
        break;
    case WS_FX_CHOICE:
        snprintf(buf, len, "%s", ws_fx_tables[p->table][clampi(v, 0, p->max)]);
        break;
    case WS_FX_CHECK:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    default:
        snprintf(buf, len, v > 0 && p->min < 0 ? "+%d" : "%d", v);
        break;
    }
}
