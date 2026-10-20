#!/usr/bin/env python3
"""Korg Wavestation SR data for src/synth_wsp.c.

Reads Edisyn's tables (reference/edisyn/edisyn/synth/korgwavestation/, Apache 2.0)
and writes, under data/:
  ws/waves        one name per line: wave number 32.. (line 1 = wave 32); 0-31 are wave sequences
  ws/wavesq/NN    names of wave sequences 0-31 per bank NN (0 RAM1 .. 11 Card)
  ws/patches/NN   names of the patches per bank (35 lines)
  ws/perfs/NN     names of the performances per bank (50 lines)
  wsp/000 Init    the init patch: a library patch is one patch as the SR keeps it, 426 bytes
                  (Edisyn's KorgWavestationPatch.init is a dump message, 852 nibbles)

    python3 tools/ws_import.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "reference", "edisyn", "edisyn", "synth", "korgwavestation")
OUT = os.path.join(ROOT, "data")


def strings(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return [s.replace("\\\"", "\"") for s in re.findall(r'"((?:[^"\\]|\\.)*)"', text)]


def array(text, name, nested):
    """The initialiser of `name`: a flat list, or a list of lists"""
    i = text.index(name)
    i = text.index("{", i)
    depth, j, groups, cur = 0, i, [], None
    while j < len(text):
        c = text[j]
        if c == "{":
            depth += 1
            if nested and depth == 2:
                cur = j + 1
        elif c == "}":
            if nested and depth == 2:
                groups.append(strings(text[cur:j]))
            depth -= 1
            if depth == 0:
                break
        j += 1
    return groups if nested else strings(text[i:j])


def write_lines(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(s + "\n" for s in lines))


def main():
    try:
        text = open(os.path.join(SRC, "KorgWavestationAbstract.java"), encoding="utf-8").read()
        init = open(os.path.join(SRC, "KorgWavestationPatch.init"), "rb").read()
    except OSError:
        sys.exit("missing reference/edisyn (git clone https://github.com/eclab/edisyn into reference/)")
    waves = array(text, "String[] WAVES", False)
    write_lines(os.path.join(OUT, "ws", "waves"), waves)
    print("waves:", len(waves))
    for name, key, count in (("wavesq", "WAVE_SEQUENCES", 32), ("patches", "PATCHES", 35),
                             ("perfs", "PERFORMANCES", 50)):
        groups = array(text, "String[][] " + key, True)
        for bank, names in enumerate(groups):
            write_lines(os.path.join(OUT, "ws", name, "%02d" % bank), names[:count])
        print(name + ":", len(groups), "banks")
    # the dump message: F0 42 3n 28 40 bank num <852 nibbles> sum F7
    if len(init) != 861 or init[4] != 0x40:
        sys.exit("unexpected init patch")
    nib = init[7:7 + 852]
    data = bytes(nib[2 * i] | nib[2 * i + 1] << 4 for i in range(426))
    os.makedirs(os.path.join(OUT, "wsp"), exist_ok=True)
    with open(os.path.join(OUT, "wsp", "000 Init"), "wb") as f:
        f.write(data)
    print("init patch:", data[:16].decode("ascii", "replace"), len(data), "bytes")


main()
