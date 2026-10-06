#!/usr/bin/env python3
"""JV-880 rhythm kit for src/synth_jv880r.c.

A kit file is one Rhythm Setup: 61 notes (36-96, C2-C7) x 52 bytes as in the
Temporary Rhythm Setup (manual p. 229-230), named "NNN Name". This writes
data/jv880r/000 Init: every key plays the first internal wave at its own
pitch with a short decaying envelope - a plain kit to start from.

    python3 tools/jv880r_import.py
"""
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "jv880r")
KEYS, NOTE_LEN = 61, 0x34


def note(key):
    n = [0] * NOTE_LEN
    n[0x03] = 1        # tone switch
    n[0x04] = key      # coarse tune: the key's own note
    n[0x07] = 64       # fine 0
    n[0x09] = 2        # bend range
    n[0x0A] = 64       # P-ENV velocity level sense 0
    n[0x0B] = 7        # P-ENV velocity time sense 0
    n[0x0C] = 64       # P-ENV depth 0
    for i in (0x0E, 0x10, 0x12, 0x14):
        n[i] = 64      # P-ENV levels 0
    n[0x15] = 1        # LPF
    n[0x16] = 127      # cutoff
    n[0x19] = 64       # TVF velocity level sense 0
    n[0x1A] = 7        # TVF velocity time sense 0
    n[0x1B] = 64       # TVF depth 0
    for i in (0x1D, 0x1F, 0x21, 0x23):
        n[i] = 127     # TVF-ENV levels
    n[0x24] = 127      # level
    n[0x25] = 4        # pan centre 64 (nibbles 4, 0)
    n[0x26] = 0
    n[0x27] = 64       # TVA velocity level sense 0
    n[0x28] = 7        # TVA velocity time sense 0
    n[0x2A] = 127      # TVA-ENV: T1 0, L1 127, T2 70, L2 0, rest 0
    n[0x2B] = 70
    n[0x30] = 127      # dry level
    n[0x31] = 40       # reverb send
    return n


def main():
    os.makedirs(OUT, exist_ok=True)
    data = bytearray()
    for k in range(KEYS):
        data += bytes(note(36 + k))
    with open(os.path.join(OUT, "000 Init"), "wb") as f:
        f.write(data)
    print("wrote", len(data), "bytes")


main()
