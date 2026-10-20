#!/usr/bin/env python3
"""U-110 tone names for src/synth_u110.c.

Reads Edisyn's tone lists (reference/edisyn/edisyn/synth/rolandu110/RolandU110.java,
Apache 2.0: the PCM[][] table, media 0 = internal ROM, 1-15 = SN-U110-01..15)
and writes data/u110/tones/NN, one tone name per line (line 1 = tone 1), for
media NN. Tones past a list's end show as numbers only.

    python3 tools/u110_names.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "reference", "edisyn", "edisyn", "synth", "rolandu110", "RolandU110.java")
OUT = os.path.join(ROOT, "data", "u110", "tones")
MEDIA = 16
NAME_MAX = 15  # what a display column holds


def table(text):
    """The PCM[][] initialiser's top-level groups, as lists of names"""
    start = text.index("String[][] PCM")
    start = text.index("{", start)
    depth, groups, cur, i = 0, [], None, start
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
            if depth == 2:
                cur = i + 1
        elif c == "}":
            if depth == 2:
                groups.append(text[cur:i])
            depth -= 1
            if depth == 0:
                break
        i += 1
    return [re.findall(r'"((?:[^"\\]|\\.)*)"', re.sub(r"//[^\n]*", "", g)) for g in groups]


def main():
    try:
        text = open(SRC, encoding="utf-8").read()
    except OSError:
        sys.exit("missing " + SRC + " (git clone https://github.com/eclab/edisyn into reference/)")
    groups = table(text)
    if len(groups) != MEDIA:
        sys.exit("expected %d media, found %d" % (MEDIA, len(groups)))
    os.makedirs(OUT, exist_ok=True)
    for n, names in enumerate(groups):
        names = [s.replace(" [SETUP]", "")[:NAME_MAX] for s in names]
        with open(os.path.join(OUT, "%02d" % n), "w") as f:
            f.write("".join(s + "\n" for s in names))
        print("%02d: %d tones" % (n, len(names)))


main()
