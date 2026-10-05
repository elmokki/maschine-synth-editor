#include <stdio.h>
#include "d110_common.h"
#include "midi.h"
#include "storage.h"

#define VOLUME_FILE STORAGE_ROOT "/state/d110_volume"

static int volume = -1;               /* -1 = not read yet */
static uint16_t channels = 0x03FE;    /* 2-10 */

int d110_volume_get(void)
{
    if (volume < 0) {
        volume = 100;
        FILE *f = fopen(VOLUME_FILE, "r");
        if (f) {
            if (fscanf(f, "%d", &volume) != 1 || volume < 0 || volume > 127) {
                volume = 100;
            }
            fclose(f);
        }
    }
    return volume;
}

void d110_volume_set(int value)
{
    volume = value;
    for (int c = 0; c < 16; c++) {
        if (channels & (1 << c)) {
            midi_send(0xB0 | c, 7, value);
        }
    }
    FILE *f = fopen(VOLUME_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", value);
        fclose(f);
    }
}

void d110_set_channels(uint16_t mask)
{
    if (mask) {
        channels = mask;
    }
}
