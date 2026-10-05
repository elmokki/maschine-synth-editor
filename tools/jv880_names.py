#!/usr/bin/env python3
"""Waveform names of the SR-JV80 expansion boards, for src/synth_jv880.c.

Reads the boards' waveform lists (PDF with a text layer, or text) from
reference/JV880/SR-JV80-NN .../ and writes data/jv880/waves/cardNN: 255
lines of 16 characters (+ newline), line n = wave number n + 1, so the
firmware finds a name by seeking to n * 17. Boards without a list get no
file (the editor shows the number only). The boards' ROM images in those
folders are never read.

    python3 tools/jv880_names.py
"""
import glob
import os
import re
import subprocess

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
REF = os.path.join(ROOT, "reference", "JV880")
OUT = os.path.join(ROOT, "data", "jv880", "waves")
WIDTH = 16

# Typos in the printed lists: (card, text as printed, fixed)
FIXES = [
    (3, r"(?<!\d)039(\s+E\. Grand 2)", r"029\1"),
    (12, r"(?<!\d)15(\s+Hip Kick)", r"152\1"),
]

# The JV-880's own waveforms (wave group INT, 1-129): manual p. 205, a
# scanned page, read by eye
INTERNAL = """Ac Piano 1|SA Rhodes 1|SA Rhodes 2|E.Piano 1|E.Piano 2|Clav 1|Organ 1|Jazz Organ|Pipe Organ|
Nylon GTR|6STR GTR|GTR HARM|Mute GTR 1|Pop Strat|Stratus|SYN GTR|Harp 1|SYN Bass|Pick Bass|E.Bass|
Fretless 1|Upright BS|Slap Bass 1|Slap & Pop|Slap Bass 2|Slap Bass 3|Flute 1|Trumpet 1|Trombone 1|
Harmon Mute1|Alto Sax 1|Tenor Sax 1|French 1|Blow Pipe|Bottle|Trumpet SECT|ST.Strings-R|
ST.Strings-L|Mono Strings|Pizz|SYN VOX 1|SYN VOX 2|Male Ooh|ORG VOX|VOX Noise|Soft Pad|JP Strings|
Pop Voice|Fine Wine|Fantasynth|Fanta Bell|ORG Bell|Agogo|Bottle Hit|Vibes|Marimba wave|Log Drum|
DIGI Bell 1|DIGI Chime|Steel Drums|MMM VOX|Spark VOX|Wave Scan|Wire String|Lead Wave|Synth Saw 1|
Synth Saw 2|Synth Saw 3|Synth Square|Synth Pulse1|Synth Pulse2|Triangle|Sine|ORG Click|White Noise|
Wind Agogo|Metal Wind|Feedbackwave|Anklungs|Wind Chimes|Rattles|Tin Wave|Spectrum 1|808 SNR 1|
90's Snare|Piccolo SN|LA Snare|Whack Snare|Rim Shot|Bright Kick|Verb Kick|Round Kick|808 Kick|
Closed HAT 1|Closed HAT 2|Open HAT 1|Crash 1|Ride 1|Ride Bell 1|Power Tom Hi|Power Tom Lo|
Cross Stick1|808 Claps|Cowbell 1|Tambourine|Timbale|CGA Mute Hi|CGA Mute Lo|CGA Slap|Conga Hi|
Conga Lo|Maracas|Cabasa Cut|Cabasa Up|Cabasa Down|REV Steel DR|REV Tin Wave|REV SN 1|REV SN 2|
REV SN 3|REV SN 4|REV Kick 1|REV Cup|REV Tom|REV Cow Bell|REV TAMB|REV Conga|REV Maracas|
REV Crash 1""".replace("\n", "").split("|")

TOKEN = re.compile(r"(?<!\S)(\d{1,3})(?=\s)")
ENTRY = re.compile(r"(?<![\w.])(\d{1,3})\s{1,8}(\S.*?)(?=\s{2,}\d{1,3}\s{1,8}\S|\s*$)")
JUNK = re.compile(r"Expansion Board|Waveform|Listing|Roland|Collection")


def text_of(path):
    if path.lower().endswith(".pdf"):
        return subprocess.run(["pdftotext", "-layout", path, "-"], capture_output=True, text=True).stdout
    return open(path, encoding="utf-8", errors="replace").read()


def by_gaps(txt):
    """Entries split at two or more spaces; fails where a list has single
    spaces between columns"""
    d = {}
    for line in txt.splitlines():
        for m in ENTRY.finditer(line):
            n = int(m.group(1))
            if 1 <= n <= 255 and n not in d:
                d[n] = m.group(2).strip()
    return d


def row(line, step):
    """A row of a list laid out in columns: its numbers go up by `step`"""
    toks = [(m.start(), m.end(), int(m.group(1))) for m in TOKEN.finditer(line)]
    if not toks or not 1 <= toks[0][2] <= 255 or toks[0][0] > 8:
        return {}
    starts, pos, want = [], 0, toks[0][2]
    for s, e, v in toks:
        if v == want and s >= pos:
            starts.append((s, e, v))
            pos, want = e, want + step
    res = {}
    for i, (s, e, v) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(line)
        if line[e:end].strip():
            res[v] = line[e:end].strip()
    return res


def by_columns(txt):
    d = {}
    for page in txt.split("\f"):
        lines = page.splitlines()
        best = (0, None)
        for step in range(5, 100):
            n = sum(len(row(l, step)) for l in lines)
            if n > best[0]:
                best = (n, step)
        if best[1]:
            for l in lines:
                for k, v in row(l, best[1]).items():
                    d.setdefault(k, v)
    return d


def tidy(name):
    """Footnote marks after the name (Latin: "...  *2")"""
    return re.sub(r"\s+\*+\d*\s*$", "", name.strip())


def clean(name):
    return len(name) <= 20 and not re.search(r"\s{2,}|\s\d{3}\s|\s\d{1,3}\s{2,}", name) and not JUNK.search(name)


def parse_table(txt):
    """Boards 98 and 99: "NN *Name   Source board" lines"""
    d = {}
    for line in txt.splitlines():
        m = re.match(r"\s*(\d{1,3})\s+\*?\s?(\S.*?)(?:\s{2,}\S.*)?$", line)
        if m and 1 <= int(m.group(1)) <= 255 and int(m.group(1)) not in d:
            d[int(m.group(1))] = m.group(2).strip()
    return d


def waveforms(card, folder):
    files = [f for f in os.listdir(folder)
             if re.search(r"(W\.pdf|Waveform|Wave List|-0?6 - Wave|sr-jv80-9[89]\.txt|SRJV80-19)", f, re.I)
             and not f.lower().endswith(".bin")]
    files = [f for f in files if f.lower().endswith((".pdf", ".txt")) and "patch" not in f.lower()]
    # 19's "SRJV80-19.pdf" is a data sheet, not a list
    files = [f for f in files if "srjv80-19" not in f.lower()]
    if not files:
        return {}
    path = os.path.join(folder, sorted(files)[0])
    txt = text_of(path)
    for c, bad, good in FIXES:
        if c == card:
            txt = re.sub(bad, good, txt)
    if card in (98, 99):
        return parse_table(txt)
    a, b = by_gaps(txt), by_columns(txt)
    d = {}
    for n in sorted(set(a) | set(b)):
        for cand in (tidy(b.get(n, "")), tidy(a.get(n, ""))):
            if cand and clean(cand):
                d[n] = cand
                break
        else:
            d[n] = min([tidy(c) for c in (a.get(n), b.get(n)) if c], key=len)
            print(f"  card {card}: wave {n} not clean: {d[n]!r}")
    # numbers past the first gap are stray (a footer's "JV-80/880/90...")
    n = 1
    while n in d:
        n += 1
    for k in [k for k in d if k > n]:
        print(f"  card {card}: dropped stray {k}: {d[k]!r}")
        del d[k]
    return d


def ascii_name(s):
    s = s.replace("’", "'").replace("‘", "'")
    s = re.sub(r"\s+", " ", s)
    return "".join(c if 32 <= ord(c) < 127 else "?" for c in s)[:WIDTH]


def write(name, d):
    lines = [ascii_name(d.get(n, "")).ljust(WIDTH) for n in range(1, 256)]
    with open(os.path.join(OUT, name), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    assert len(INTERNAL) == 129, len(INTERNAL)
    write("int", {i + 1: n for i, n in enumerate(INTERNAL)})
    print("int: 129 names")
    for folder in sorted(glob.glob(os.path.join(REF, "SR-JV80-*"))):
        card = int(re.match(r"SR-JV80-(\d+)", os.path.basename(folder)).group(1))
        d = waveforms(card, folder)
        if not d:
            print(f"card {card:02d}: no waveform list")
            continue
        write(f"card{card:02d}", d)
        missing = [n for n in range(1, max(d) + 1) if n not in d]
        print(f"card {card:02d}: {len(d)} names (to {max(d)})" + (f", missing {missing[:8]}" if missing else ""))
