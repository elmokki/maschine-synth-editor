#!/usr/bin/env python3
"""JV-880 performance for src/synth_jv880p.c.

A performance file is one performance: 311 bytes as in the Temporary
Performance (common 31 + 8 parts x 35, manual p. 224-225), named "NNN Name".
This writes data/jv880p/000 Init, a plain performance to start from: parts
1-7 play the internal patches I01-I07 on channels 1-7, part 8 is the rhythm
part (internal kit) on channel 10.

    python3 tools/jv880p_import.py
"""
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "jv880p")


def common():
    c = [0] * 0x1F
    c[0x00:0x0C] = list(b"Init Perf   ")
    c[0x0D] = 3        # reverb STAGE2
    c[0x0E] = 100      # reverb level
    c[0x0F] = 60       # reverb time
    c[0x11] = 0        # chorus 1
    c[0x12] = 60       # chorus level
    c[0x13] = 80       # chorus depth
    c[0x14] = 60       # chorus rate
    for i in range(7):
        c[0x17 + i] = 3    # voice reserves: 7 x 3 + 7 = 28
    c[0x1E] = 7
    return c


def part(n):
    p = [0] * 0x23
    p[0x15] = 1                    # receive switch
    p[0x16] = n if n < 7 else 9    # receive channel
    p[0x18] = n if n < 7 else 0    # patch number I01.. (low nibble)
    p[0x19] = 127                  # level
    p[0x1A] = 64                   # pan centre
    p[0x1B] = 64                   # coarse 0
    p[0x1C] = 64                   # fine 0
    p[0x1D] = p[0x1E] = 1          # reverb, chorus on
    p[0x1F] = p[0x20] = p[0x21] = 1  # receive program change, volume, hold-1
    return p


def main():
    os.makedirs(OUT, exist_ok=True)
    data = bytes(common())
    for n in range(8):
        data += bytes(part(n))
    with open(os.path.join(OUT, "000 Init"), "wb") as f:
        f.write(data)
    print("wrote", len(data), "bytes")


main()
