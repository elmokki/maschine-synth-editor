#include "jv880_common.h"

static void put_addr(uint8_t *p, uint32_t v)
{
    p[0] = v >> 21 & 0x7F;
    p[1] = v >> 14 & 0x7F;
    p[2] = v >> 7 & 0x7F;
    p[3] = v & 0x7F;
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += p[i];
    }
    return (128 - (sum & 0x7F)) & 0x7F;
}

static size_t header(int dev, uint8_t command, uint32_t addr, uint8_t *out)
{
    out[0] = 0xF0;
    out[1] = 0x41;
    out[2] = dev & 0x7F;
    out[3] = JV880_MODEL_ID;
    out[4] = command;
    put_addr(out + 5, addr);
    return 9;
}

size_t jv880_dt1(int dev, uint32_t addr, const uint8_t *data, size_t len, uint8_t *out)
{
    size_t n = header(dev, 0x12, addr, out);
    for (size_t i = 0; i < len; i++) {
        out[n++] = data[i] & 0x7F;
    }
    out[n] = checksum(out + 5, n - 5);
    n++;
    out[n++] = 0xF7;
    return n;
}

size_t jv880_rq1(int dev, uint32_t addr, uint32_t size, uint8_t *out)
{
    size_t n = header(dev, 0x11, addr, out);
    put_addr(out + n, size);
    n += 4;
    out[n] = checksum(out + 5, n - 5);
    n++;
    out[n++] = 0xF7;
    return n;
}

int jv880_parse_dt1(const uint8_t *d, size_t len, int dev, uint32_t *addr, const uint8_t **data)
{
    /* F0 41 dev 46 12 a a a a data... sum F7 */
    if (len < 11 || d[0] != 0xF0 || d[1] != 0x41 || d[2] != (dev & 0x7F) || d[3] != JV880_MODEL_ID ||
        d[4] != 0x12 || d[len - 1] != 0xF7) {
        return -1;
    }
    int sum = 0;
    for (size_t i = 5; i < len - 1; i++) {
        sum += d[i];
    }
    if (sum & 0x7F) {
        return -1;
    }
    *addr = JV880_ADDR(d[5], d[6], d[7], d[8]);
    *data = d + 9;
    return (int)len - 11;
}
