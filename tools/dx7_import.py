#!/usr/bin/env python3
"""DX7 voices for the Volca FM2 driver (src/synth_volca.c).

A patch file is one voice as the DX7's single-voice edit data (VCED,
155 bytes, the format of a 1-voice SysEx dump without its header/checksum),
named "NNN Name". This writes:
  000 Init      - the DX7's INIT VOICE
  001..         - the voices of the given 32-voice banks (.syx, packed VMEM),
                  in order
into data/dx7/. Banks are third-party data: keep them in reference/ (not
committed); data/ isn't committed either.

    python3 tools/dx7_import.py reference/DX7/dx7patch/ROM1A.SYX
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "dx7")
VCED_LEN = 155


def init_voice():
    v = []
    for op in range(6):  # stored OP6 first
        v += [99, 99, 99, 99, 99, 99, 99, 0,   # EG rates, levels
              39, 0, 0, 0, 0,                  # break point, depths, curves
              0, 0, 0,                         # rate scaling, AMS, KVS
              99 if op == 5 else 0,            # output level: OP1 only
              0, 1, 0, 7]                      # ratio mode, coarse 1, fine, detune 0
    v += [99, 99, 99, 99, 50, 50, 50, 50]      # pitch EG
    v += [0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24]   # alg 1, fb, OKS, LFO..., transpose C3
    v += list(b"INIT VOICE")
    assert len(v) == VCED_LEN
    return bytes(v)


def unpack(p):
    """One 128-byte packed VMEM voice -> 155-byte VCED"""
    v = []
    for op in range(6):
        o = p[op * 17:(op + 1) * 17]
        v += list(o[0:11])
        v += [o[11] & 3, (o[11] >> 2) & 3,      # left curve, right curve
              o[12] & 7,                         # rate scaling
              o[13] & 3, (o[13] >> 2) & 7,      # AMS, KVS
              o[14],                             # output level
              o[15] & 1, (o[15] >> 1) & 31,     # mode, coarse
              o[16],                             # fine
              (o[12] >> 3) & 15]                 # detune
    v += list(p[102:110])                        # pitch EG
    v += [p[110] & 31, p[111] & 7, (p[111] >> 3) & 1,
          p[112], p[113], p[114], p[115],
          p[116] & 1, (p[116] >> 1) & 7, (p[116] >> 4) & 7,
          p[117]]
    v += [c & 0x7F for c in p[118:128]]
    assert len(v) == VCED_LEN
    return bytes(v)


def bank_voices(path):
    d = open(path, "rb").read()
    i = d.find(b"\xF0\x43")
    if i < 0 or d[i + 3] != 9:
        sys.exit(f"{path}: not a DX7 32-voice dump")
    data = d[i + 6:i + 6 + 4096]
    return [unpack(data[n * 128:(n + 1) * 128]) for n in range(32)]


def file_name(num, voice):
    name = voice[145:155].decode("ascii", "replace").strip()
    name = re.sub(r" +", " ", re.sub(r"[^A-Za-z0-9 .+\-]", "_", name)) or "VOICE"
    return f"{num:03d} {name}"


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        os.remove(os.path.join(OUT, f))
    voices = [("000 Init", init_voice())]
    for bank in sys.argv[1:]:
        for v in bank_voices(bank):
            voices.append((file_name(len(voices), v), v))
    seen = set()
    for name, v in voices:
        while name.lower() in seen:  # LittleFS names must differ
            name += "_"
        seen.add(name.lower())
        open(os.path.join(OUT, name), "wb").write(v)
    print(f"{len(voices)} voices -> {OUT}")
    print(", ".join(n for n, _ in voices[:40]))
