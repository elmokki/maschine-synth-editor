#!/usr/bin/env python3
"""U-110 init patch for src/synth_u110.c.

A patch file is one patch, 171 bytes, named "NNN Name": the name (10 ASCII
characters), the 5 common parameters (output mode, chorus rate/depth,
tremolo rate/depth) and 6 parts x 26 parameters, each in the order of its
individual-parameter address (manual p. 90: 00 1n 00 .. 00 1n 19). This writes
data/u110/000 Init: six parts on channels 1-6 (part 6 the drums on 10),
each playing a tone of the internal ROM.

    python3 tools/u110_import.py      (after tools/u110_names.py)
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "u110")
NAMES = os.path.join(OUT, "tones", "00")

# part n: (tone name in the internal ROM, receive channel 0-15)
PARTS = [("A. Piano 1", 0), ("E. Piano 1", 1), ("A. Guitar 1", 2), ("AC. Bass", 3), ("Strings 1", 4),
         ("Drums", 9)]


def part(tone, ch):
    p = [0] * 26
    p[0x00] = 0        # output assign: 1
    p[0x01] = ch       # receive channel
    p[0x02] = 0        # tone media: internal
    p[0x03] = tone     # tone number (0 = tone 1)
    p[0x04] = 2        # bend range
    p[0x05] = 0        # key range lo
    p[0x06] = 127      # key range hi
    p[0x07] = 127      # part level
    p[0x08] = 8        # velocity sens
    p[0x0A] = p[0x0B] = 8  # attack, release rate: 0
    p[0x0C] = p[0x0D] = 64  # coarse, fine: 0
    p[0x18] = 8        # pitch poly press sens: 0 semitones
    return p


def main():
    try:
        names = [s.strip() for s in open(NAMES)]
    except OSError:
        sys.exit("run tools/u110_names.py first")
    data = list(b"Init Patch")
    data += [0, 8, 8, 8, 8]  # output mode 1, chorus/tremolo
    for name, ch in PARTS:
        data += part(names.index(name), ch)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "000 Init"), "wb") as f:
        f.write(bytes(data))
    print("wrote", len(data), "bytes")


main()
