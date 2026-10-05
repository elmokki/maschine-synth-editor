#!/usr/bin/env python3
"""D-110 tones for src/synth_d110.c.

A patch file is one tone: 246 bytes as in the D-110's Tone Temporary Area
(common 14 + 4 partials x 58, manual p. 117), named "NNN Name". This writes
data/d110/000 Init, a plain tone to start from: partial 1 only, a square
wave through an open filter.

    python3 tools/d110_import.py
"""
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "d110")


def partial():
    p = [0] * 0x3A
    p[0x00] = 36       # coarse C4
    p[0x01] = 50       # fine 0
    p[0x02] = 11       # key follow 1
    p[0x03] = 1        # bender on
    p[0x04] = 0        # square, bank 1
    p[0x06] = 0        # pulse width
    p[0x07] = 7        # PW velo 0
    p[0x0F] = p[0x10] = p[0x11] = p[0x12] = p[0x13] = 50  # pitch env levels 0
    p[0x17] = 100      # cutoff open
    p[0x19] = 11       # TVF key follow 1
    p[0x1A] = 64       # bias point >A1
    p[0x1B] = 7        # bias level 0
    p[0x25] = p[0x26] = p[0x27] = p[0x28] = 100   # TVF env levels
    p[0x29] = 100      # TVA level
    p[0x2A] = 50       # velocity 0
    p[0x2B] = p[0x2D] = 64
    p[0x2C] = p[0x2E] = 12   # bias levels 0
    p[0x31:0x36] = [0, 0, 0, 0, 30]          # times, release 30
    p[0x36] = p[0x37] = p[0x38] = p[0x39] = 100   # levels, sustain
    return p


def init_tone():
    t = list(b"Init Tone ")
    t += [0, 0, 0x01, 0]   # structure 1 & 1, only partial 1 sounds, env normal
    for _ in range(4):
        t += partial()
    assert len(t) == 0xF6
    return bytes(t)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "000 Init"), "wb").write(init_tone())
    print("data/d110/000 Init")
