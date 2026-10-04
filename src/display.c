#include <string.h>
#include "display.h"

#define STRIDE (DISPLAY_WIDTH / 8)
#define CHUNK_ROWS 8
#define CHUNKS (DISPLAY_HEIGHT / CHUNK_ROWS)

static uint8_t fb[DISPLAY_COUNT][STRIDE * DISPLAY_HEIGHT];
static uint8_t dirty[DISPLAY_COUNT];  /* bit n = chunk n needs sending */

static void mark_rows(int disp, int y0, int y1)
{
    for (int c = y0 / CHUNK_ROWS; c <= y1 / CHUNK_ROWS; c++) {
        dirty[disp] |= 1 << c;
    }
}

void display_clear(int disp)
{
    memset(fb[disp], 0, sizeof(fb[disp]));
    dirty[disp] = 0xFF;
}

static inline void put_pixel(int disp, int x, int y, bool on)
{
    uint8_t *p = &fb[disp][STRIDE * y + (x >> 3)];
    uint8_t bit = 0x80 >> (x & 7);
    if (on) {
        *p |= bit;
    } else {
        *p &= ~bit;
    }
}

void display_pixel(int disp, int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    put_pixel(disp, x, y, on);
    mark_rows(disp, y, y);
}

void display_fill_rect(int disp, int x, int y, int w, int h, bool on)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > DISPLAY_WIDTH ? DISPLAY_WIDTH : x + w;
    int y1 = y + h > DISPLAY_HEIGHT ? DISPLAY_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            put_pixel(disp, xx, yy, on);
        }
    }
    mark_rows(disp, y0, y1 - 1);
}

void display_rect(int disp, int x, int y, int w, int h, bool on)
{
    display_fill_rect(disp, x, y, w, 1, on);
    display_fill_rect(disp, x, y + h - 1, w, 1, on);
    display_fill_rect(disp, x, y, 1, h, on);
    display_fill_rect(disp, x + w - 1, y, 1, h, on);
}

void display_round_rect(int disp, int x, int y, int w, int h, bool filled, bool on)
{
    if (filled) {
        display_fill_rect(disp, x, y, w, h, on);
        /* Cut a 3-pixel corner off each corner */
        static const int8_t cut[3][2] = {{0, 0}, {1, 0}, {0, 1}};
        for (int i = 0; i < 3; i++) {
            int dx = cut[i][0], dy = cut[i][1];
            display_pixel(disp, x + dx, y + dy, !on);
            display_pixel(disp, x + w - 1 - dx, y + dy, !on);
            display_pixel(disp, x + dx, y + h - 1 - dy, !on);
            display_pixel(disp, x + w - 1 - dx, y + h - 1 - dy, !on);
        }
        return;
    }
    display_fill_rect(disp, x + 2, y, w - 4, 1, on);
    display_fill_rect(disp, x + 2, y + h - 1, w - 4, 1, on);
    display_fill_rect(disp, x, y + 2, 1, h - 4, on);
    display_fill_rect(disp, x + w - 1, y + 2, 1, h - 4, on);
    display_pixel(disp, x + 1, y + 1, on);
    display_pixel(disp, x + w - 2, y + 1, on);
    display_pixel(disp, x + 1, y + h - 2, on);
    display_pixel(disp, x + w - 2, y + h - 2, on);
}

void display_line(int disp, int x0, int y0, int x1, int y1, bool on)
{
    /* Bresenham */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        display_pixel(disp, x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static const glyph_t *glyph_for(const font_t *font, char c)
{
    unsigned char uc = (unsigned char)c;
    if (uc < font->first || uc > font->last) {
        uc = '?';
    }
    return &font->glyphs[uc - font->first];
}

int display_text_width(const font_t *font, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        w += glyph_for(font, *s)->advance;
    }
    return w;
}

int display_text(int disp, int x, int y, const font_t *font, const char *s, bool on)
{
    for (; *s; s++) {
        const glyph_t *g = glyph_for(font, *s);
        const uint8_t *bits = &font->bitmap[g->offset];
        int stride = (g->width + 7) / 8;
        for (int gy = 0; gy < font->height; gy++) {
            int py = y + gy;
            if (py < 0 || py >= DISPLAY_HEIGHT) {
                continue;
            }
            for (int gx = 0; gx < g->width; gx++) {
                int px = x + gx;
                if (px >= 0 && px < DISPLAY_WIDTH &&
                    (bits[gy * stride + (gx >> 3)] & (0x80 >> (gx & 7)))) {
                    put_pixel(disp, px, py, on);
                }
            }
        }
        x += g->advance;
    }
    int y0 = y < 0 ? 0 : y;
    int y1 = y + font->height > DISPLAY_HEIGHT ? DISPLAY_HEIGHT - 1 : y + font->height - 1;
    if (y0 <= y1) {
        mark_rows(disp, y0, y1);
    }
    return x;
}

void display_invalidate_all(void)
{
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        dirty[d] = 0xFF;
    }
}

bool display_next_packet(uint8_t *out)
{
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        for (int c = 0; c < CHUNKS; c++) {
            if (!(dirty[d] & (1 << c))) {
                continue;
            }
            dirty[d] &= ~(1 << c);
            const uint8_t header[9] = {
                0xE0 + d, 0x00, 0x00, c * CHUNK_ROWS, 0x00, 0x20, 0x00, 0x08, 0x00,
            };
            memcpy(out, header, sizeof(header));
            memcpy(out + sizeof(header), &fb[d][c * CHUNK_ROWS * STRIDE], CHUNK_ROWS * STRIDE);
            return true;
        }
    }
    return false;
}
