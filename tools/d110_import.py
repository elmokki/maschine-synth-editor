#!/usr/bin/env python3
"""D-110 tones for src/synth_d110.c.

A patch file is one tone: 246 bytes as in the D-110's Tone Temporary Area
(common 14 + 4 partials x 58, manual p. 117), named "NNN Name". This writes
data/d110/000 Init, a plain tone to start from: partial 1 only, a square
wave through an open filter; and data/d110r/000 Factory, the factory
Rhythm Setup (src/synth_d110r.c: 85 keys x 4 bytes).

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


# The factory Rhythm Setup (manual p. 102, "Initial Rhythm Setting"): the
# r tone per key from C1 (24) to C8 (108); r64 = OFF
FACTORY_RHYTHM = {35: 15, 36: 16, 37: 25, 38: 19, 39: 39, 40: 20, 41: 30, 42: 1, 43: 33, 44: 4,
                  45: 29, 46: 3, 47: 32, 48: 28, 49: 5, 50: 31, 51: 8, 52: 13, 53: 11, 54: 40,
                  55: 14, 56: 41, 57: 7, 58: 21, 59: 10, 60: 42, 61: 43, 62: 44, 63: 45, 64: 46,
                  65: 47, 66: 48, 67: 49, 68: 50, 69: 51, 70: 52, 71: 53, 72: 54, 73: 55, 74: 12,
                  75: 56, 76: 26, 77: 27, 78: 57, 79: 38, 80: 58, 81: 37, 82: 59, 83: 60, 84: 17,
                  85: 18, 86: 22, 87: 23, 88: 24, 89: 36, 90: 2, 91: 35, 92: 6, 93: 34, 94: 9,
                  95: 61, 96: 62, 97: 63}


def factory_rhythm():
    """85 keys x (tone, level, pan, output); tone 64 + n - 1 = r<n>. The
    manual only lists the tones: level full, pan centre, output mix."""
    d = []
    for key in range(24, 109):
        d += [64 + FACTORY_RHYTHM.get(key, 64) - 1, 100, 7, 0]
    return bytes(d)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "000 Init"), "wb").write(init_tone())
    print("data/d110/000 Init")
    rhythm_dir = os.path.join(ROOT, "data", "d110r")
    os.makedirs(rhythm_dir, exist_ok=True)
    open(os.path.join(rhythm_dir, "000 Factory"), "wb").write(factory_rhythm())
    print("data/d110r/000 Factory")
