#!/usr/bin/env python3
"""Wavestation SR effects tables for synth_wsperf.c, generated from the cloned Edisyn
(reference/edisyn/edisyn/synth/korgwavestation/, Apache 2.0): effect types, groups, how each parameter is
packed into the 8-byte effect block (FX_PCL_LIST), the factory presets (FX_PRESETS) and the labels, ranges
and value texts Edisyn's editor uses (buildFX in KorgWavestationPerformance.java).

  python tools/ws_fx_gen.py            writes src/ws_fx_data.c
  python tools/ws_fx_gen.py --check    lists what could not be mapped, writes nothing

The generated file is committed (reference/ is not).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "reference/edisyn/edisyn/synth/korgwavestation")
OUT = os.path.join(ROOT, "src/ws_fx_data.c")

perf = open(os.path.join(DIR, "KorgWavestationPerformance.java"), encoding="utf-8").read()
abstract = open(os.path.join(DIR, "KorgWavestationAbstract.java"), encoding="utf-8").read()


def strip_comments(s):
    return re.sub(r"//[^\n]*", "", s)


def block(src, start_pat, open_ch="{", close_ch="}"):
    """The text of the brace block that follows start_pat's match."""
    m = re.search(start_pat, src)
    i = src.index(open_ch, m.end())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == open_ch:
            depth += 1
        elif src[j] == close_ch:
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
    raise ValueError(start_pat)


def ints(s):
    return [int(x) for x in re.findall(r"-?\d+", s)]


def strings(src, name):
    body = block(src, r"String\[\]\s+" + name + r"\s*=\s*new String\[\]\s*")
    return re.findall(r'"((?:[^"\\]|\\.)*)"', strip_comments(body))


# ---- types ----
FX_NAMES = strings(abstract, "FX")
FX_GROUP = ints(strip_comments(block(perf, r"static final int FX_GROUP\[\]\s*=\s*new int\[\]")))
assert len(FX_NAMES) == len(FX_GROUP) == 56, (len(FX_NAMES), len(FX_GROUP))
NGROUPS = max(FX_GROUP) + 1

# ---- FX_INDICES: per group, the paramnum of each UI parameter index ----
idx_body = strip_comments(block(perf, r"int\[\]\[\] FX_INDICES\s*=\s*new int\[\]\[\]"))
FX_INDICES = [ints(g) for g in re.findall(r"\{([^{}]*)\}", idx_body)]
assert len(FX_INDICES) == NGROUPS, len(FX_INDICES)

# FX_INDICES has mistakes (Stereo/Ping-Pong Delay's feedback says 3, the bit table and FX_PARAMETERS say 13): take
# the numbers from FX_PARAMETERS (FX_PARAM13, FX_UPARAM4, FX_MOD1, FX_100_WET_DRY0 ...) and report where they differ
par_body = strip_comments(block(perf, r"int\[\]\[\] FX_PARAMETERS\s*=\s*new int\[\]\[\]"))
FX_PARAMETERS = []
for g, idx in zip(re.findall(r"\{([^{}]*)\}", par_body), FX_INDICES):
    row = []
    for i, t in enumerate(re.findall(r"FX_\w+", g)):
        m = re.search(r"(\d+)$", t)
        row.append(int(m.group(1)) if m else idx[i])  # FX_LFO_SHAPE has no number
    FX_PARAMETERS.append(row)
assert len(FX_PARAMETERS) == NGROUPS, len(FX_PARAMETERS)
for g in range(NGROUPS):
    if len(FX_PARAMETERS[g]) != len(FX_INDICES[g]):
        FX_PARAMETERS[g] = FX_INDICES[g]  # the compressor's control source has no parameter id
    if FX_PARAMETERS[g] != FX_INDICES[g]:
        print("note: group %d FX_INDICES %s differs from FX_PARAMETERS %s (using the latter)" % (
            g, FX_INDICES[g], FX_PARAMETERS[g]))
FX_INDICES = FX_PARAMETERS

# ---- FX_PCL_LIST: per group, rows (byte, bit, len, paramnum, parambit) ----
pcl_body = strip_comments(block(perf, r"int\[\]\[\]\[\] FX_PCL_LIST\s*=\s*new int\[\]\[\]\[\]"))
groups_txt = re.findall(r"\{\s*((?:\{[^{}]*\}\s*,?\s*)*)\}", pcl_body)
FX_PCL = [[ints(r) for r in re.findall(r"\{([^{}]*)\}", g)] for g in groups_txt]
FX_PCL = [[]] + FX_PCL[1:]  # entry 0 is the header (mix 3/4), kept by the driver itself; group 0 = no effect
assert len(FX_PCL) == NGROUPS, len(FX_PCL)

# A parameter number the bit table does not have, where it has exactly one the UI does not use (the footswitch
# of Chorus/Flanger-Stereo Delay: p6 in the lists, p7 in the table): that one
for g in range(NGROUPS):
    nums = {r[3] for r in FX_PCL[g]}
    missing = [n for n in FX_INDICES[g] if n not in nums]
    spare = sorted(nums - set(FX_INDICES[g]))
    if len(missing) == 1 and len(spare) == 1:
        print("note: group %d parameter %d is %d in the bit table" % (g, missing[0], spare[0]))
        FX_INDICES[g] = [spare[0] if n == missing[0] else n for n in FX_INDICES[g]]

# ---- presets: one value per PCL row of the type's group ----
pre_body = strip_comments(block(perf, r"int\[\]\[\] FX_PRESETS\s*=\s*new int\[\]\[\]"))
FX_PRESETS = [ints(g) for g in re.findall(r"\{([^{}]*)\}", pre_body)]
assert len(FX_PRESETS) == 56, len(FX_PRESETS)
for t in range(56):
    n = len(FX_PCL[FX_GROUP[t]])
    assert len(FX_PRESETS[t]) == n, (t, FX_NAMES[t], len(FX_PRESETS[t]), n)


def preset_block(t):
    out = bytearray(8)
    for row, value in zip(FX_PCL[FX_GROUP[t]], FX_PRESETS[t]):
        byte, bit, ln = row[0], row[1], abs(row[2])
        out[byte] |= ((value & ((1 << ln) - 1)) << bit) & 0xFF
    return out


# ---- value texts ----
CHORUS_RATE = strings(perf, "CHORUS_LFO_RATE")
SPLIT = strings(perf, "SPLIT_POINTS")
FREQ = strings(perf, "FREQUENCIES")
LFO_SHAPES = strings(perf, "LFO_SHAPES")
FACTORS = strings(perf, "FACTORS")
SHORT = {
    "FX_SOURCES": ["NONE", "MOD WHEEL", "AFTERTCH", "LAST VEL", "HIGH VEL", "AMP ENVS", "KEY GATE", "FX SWITCH",
                   "FX TOGGLE", "MOD PEDAL", "MIDI 1", "MIDI 2", "AT+MODWHL", "JOY AC", "JOY BD"],
    "CONTROL_SOURCES": ["NORMAL", "BUS A+B", "BUS C+D", "BUS ABCD"],
    "VOCODER_BUSSES": ["A", "B", "C", "D"],
    "FACTORS": FACTORS,
}
TABLES = ["FX_SOURCES", "CONTROL_SOURCES", "VOCODER_BUSSES", "FACTORS"]
assert len(strings(abstract, "FX_SOURCES")) == 15

TYPE_SHORT = [
    "OFF", "SM HALL", "MED HALL", "LRG HALL", "SM ROOM", "LRG ROOM", "LIVE STG", "WET PLATE", "DRY PLATE",
    "SPRING", "ER 1", "ER 2", "ER 3", "GATED REV", "REV GATE", "ST DELAY", "PINGPONG", "DUAL DLY",
    "MTAP DLY 1", "MTAP DLY 2", "MTAP DLY 3", "ST CHORUS", "QUAD CHO", "XOVER CHO", "HARM CHO", "ST FLANG 1",
    "ST FLANG 2", "XOVER FLG", "ENHANCER", "DISTORT", "OVERDRIVE", "PHASER 1", "PHASER 2", "ROTARY",
    "ST MODPAN", "QUAD PAN", "PARAM EQ", "CHO+DLY", "FLG+DLY", "DLY+HALL", "DLY+ROOM", "DLY+CHO", "DLY+FLG",
    "DLY+DIST", "DLY+OVRDR", "DLY+PHASE", "DLY+ROTRY", "PITCH SHFT", "PS DELAY", "COMPRESS", "VOCODER 1",
    "VOCODER 2", "VOCODER 3", "VOCODER 4", "ST VOC 1", "ST VOC 2",
]
assert len(TYPE_SHORT) == 56 and all(len(s) <= 10 for s in TYPE_SHORT)

# ---- the UI: label, range, kind, value text per parameter, in Edisyn's left-to-right order ----
ui_start = perf.index("public VBox[] buildFX(")
ui_end = perf.index("public JComponent addFX(")
ui = strip_comments(perf[ui_start:ui_end])

ABBR = [
    ("Frequency", "FREQ"), ("Modulation", "MOD"), ("Modulator", "MODLTR"), ("Amount", "AMT"), ("Source", "SRC"),
    ("Threshold", "THRESH"), ("Feedback", "FDBK"), ("Resonance", "RESO"), ("Compression", "COMP"),
    ("Reverb", "REV"), ("Chorus", "CHO"), ("Flanger", "FLG"), ("Phaser", "PHS"), ("Delay", "DLY"),
    ("Level", "LVL"), ("Damping", "DAMP"), ("Reflect", "REFL"), ("Sensitivity", "SENS"), ("Distortion", "DIST"),
    ("Output", "OUT"), ("Stereo", "ST"), ("Excitation", "EXCITE"), ("Acceleration", "ACCEL"),
    ("Crossover", "XOVER"), ("Sibilance", "SIBIL"), ("Pre-Delay", "PRE DLY"), ("Pre Delay", "PRE DLY"),
    ("Depth", "DEPTH"), ("Carrier", "CARR"), ("Control", "CTRL"), ("Center", "CTR"), ("Width", "WIDTH"),
    ("Footswitch", "FOOTSW"), ("Sample", "SMPL"), ("Sweep", "SWEEP"), ("Range", "RANGE"), ("Ratio", "RATIO"),
    ("Shift", "SHIFT"), ("Scaler", "SCALER"), ("Slow", "SLOW"), ("Speed", "SPEED"),
]
OVERRIDES = {
    "Dry/Wet": "DRY/WET", "Mix Mod": "MIX MOD", "Mix Mod Amount": "MIXMOD AMT", "Mix Mod Source": "MIXMOD SRC",
    "Input Level Mod Source": "IN LVL SRC", "Delay Mod Source": "DLYMOD SRC", "Delay Mod Amount": "DLYMOD AMT",
    "Gate Key Mod Source": "GATE SRC", "Dry/Wet Mod Source": "MIXMOD SRC", "Dry/Wet Mod Amount": "MIXMOD AMT",
    "LFO Rate Mod Source": "RATE SRC", "LFO Rate Mod Amount": "RATE MOD",
    "LFO Depth Mod Source": "DEPTH SRC", "LFO Depth Mod Amount": "DEPTH MOD",
    "Ramp Speed Mod Source": "RAMP SRC", "Hot Spot Mod Source": "HOTSPT SRC", "Hot Spot Mod Amount": "HOTSPT MOD",
    "Output Level Mod Source": "OUTLVL SRC", "Speed Mod Source": "SPEED SRC",
    "Mid Frequency Mod Source": "MIDFRQ SRC", "Mid Frequency Mod Amount": "MIDFRQ MOD",
    "Mix Modulation Source": "MIXMOD SRC", "Mix Modulation": "MIXMOD AMT", "Shift Scaler Modulation Source": "SCALE SRC",
    "Control Source": "CONTROL", "Modulator Bus": "MODUL BUS", "Carrier Bus": "CARR BUS",
    "Sibilance Mod Source": "SIBIL SRC", "Sibilance Mod Amount": "SIBIL MOD", "Sibilance": "SIBILANCE",
    "Footswitch Sample": "FS SAMPLE", "Delay Factor": "DLY FACTOR", "Input Level": "IN LEVEL",
    "Gate Hold Time": "GATE HOLD", "Gate Threshold": "GATE THR", "Early Reflect": "EARLY REF",
    "HiFreq Damping": "HF DAMP", "Reverb High Frequency Damping": "REV DAMP", "Reverb Decay Time": "REV DECAY",
    "Reverb Pre-Delay": "REV PREDLY", "Decay Time": "DECAY", "Pre Delay": "PRE DELAY", "Delay Time": "DLY TIME",
    "Delay Feedback": "DLY FDBK", "Delay Left": "DLY LEFT", "Delay Right": "DLY RIGHT", "Delay 1": "DELAY 1",
    "Delay 2": "DELAY 2", "Delay A": "DELAY A", "Delay B": "DELAY B", "Feedback A": "FDBK A", "Feedback B": "FDBK B",
    "Chorus Delay": "CHO DELAY", "Chorus Delay Time": "CHO DELAY", "Chorus LFO Depth": "CHO DEPTH",
    "Chorus LFO Rate": "CHO RATE", "Chorus Feedback": "CHO FDBK", "Flanger Delay": "FLG DELAY",
    "Flanger Delay Time": "FLG DELAY", "Flanger LFO": "FLG LFO", "Flanger LFO Depth": "FLG DEPTH",
    "Flanger LFO Rate": "FLG RATE", "Flanger Feedback": "FLG FDBK", "Flanger": "FLG FDBK",
    "Phaser Center": "PHS CENTER", "Phaser LFO Depth": "PHS DEPTH", "Phaser LFO Rate": "PHS RATE",
    "Phaser Feedback": "PHS FDBK", "Distortion Output Level": "DIST OUT", "Output Level": "OUT LEVEL",
    "Compression Ratio": "COMP RATIO", "Compression": "COMPRESS", "Center Frequency": "CENTER", "Max Shift": "MAX SHIFT",
    "Shift Scaler": "SHIFT SCL", "Pitch Shift": "PITCH", "Crossover Delay": "XOVER DLY", "Stereo Width": "ST WIDTH",
    "Sweep Range": "SWEEP", "LFO Sweep Range": "SWEEP", "Top Delay": "TOP DELAY", "Output Mix": "OUT MIX",
    "High Frequency": "HIGH FREQ", "High Level": "HIGH LVL", "Mid Frequency": "MID FREQ", "Mid Level": "MID LVL",
    "Mid Width": "MID WIDTH", "Low Frequency": "LOW FREQ", "Low Level": "LOW LVL", "Split Point": "SPLIT PT",
    "LFO Depth": "LFO DEPTH", "LFO Rate": "LFO RATE", "LFO Shape": "LFO SHAPE", "Ramp Speed": "RAMP SPD",
    "Slow Speed": "SLOW SPD", "Fast Speed": "FAST SPD", "Hot Spot": "HOT SPOT", "Gate": "GATE",
    "Modulator Bus": "MODUL BUS", "Resonance": "RESONANCE", "Feedback": "FEEDBACK", "Excitation": "EXCITE",
    "Acceleration": "ACCEL", "Footswitch": "FOOTSW", "Delay Level": "DLY LEVEL", "Depth": "DEPTH",
    "Edge": "EDGE", "Sensitivity": "SENSITIV", "Early Reflect": "EARLY REF", "EQ Low": "EQ LOW", "EQ High": "EQ HIGH",
}


def short_label(label):
    if label in OVERRIDES:
        return OVERRIDES[label]
    s = label
    for a, b in ABBR:
        s = s.replace(a, b)
    return s.upper()


def mix_label(suffix):
    suffix = suffix.strip()
    return {"": "MIX", "A": "MIX A", "B": "MIX B", "(Delay)": "DLY MIX", "(Reverb)": "REV MIX"}[suffix]


UI = {g: [] for g in range(NGROUPS)}   # group -> list of dicts, in UI order
cuts = [(int(m.group(1)), m.start()) for m in re.finditer(r"fx\[(\d+)\]\s*=\s*main;", ui)]
cuts.append((None, len(ui)))
tok = re.compile(
    r'buildFXDryWetMix(Small)?\("fx"\s*\+\s*val\s*\+\s*"class(\d+)param(\d+)"(?:,\s*"([^"]*)")?,\s*color\)'
    r'|new LabelledDial\("([^"]*)",\s*this,\s*"fx"\s*\+\s*val\s*\+\s*"class(\d+)param(\d+)",\s*color,\s*(-?\d+),\s*(-?\d+)\)'
    r'|new Chooser\("([^"]*)",\s*this,\s*"fx"\s*\+\s*val\s*\+\s*"class(\d+)param(\d+)",\s*(\w+)\)'
    r'|new CheckBox\("([^"]*)",\s*this,\s*"fx"\s*\+\s*val\s*\+\s*"class(\d+)param(\d+)"\)'
    r'|params\s*=\s*(\w+);')
FMT = {"NUM": 0, "MIX100": 1, "MIX11": 2, "PM10": 3, "RATE": 4, "RATE_FIXED": 5, "SHAPE": 6, "SPLIT": 7, "FREQ": 8,
       "MANUAL": 9, "CHOICE": 10, "CHECK": 11}
for (g, start), (_, end) in zip(cuts, cuts[1:]):
    seg = ui[start:end]
    params_var = None
    for m in tok.finditer(seg):
        if m.group(0).startswith("params"):
            params_var = m.group(17)
            continue
        rest = seg[m.end():m.end() + 700]
        rest = rest.split("hbox.add(comp)")[0]
        if m.group(0).startswith("buildFX"):
            small, cls, idx, suffix = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4) or ""
            p = dict(idx=idx, label=mix_label(suffix), min=1 if small else 0, max=11 if small else 100,
                     fmt="MIX11" if small else "MIX100", table=-1)
        elif m.group(5) is not None:
            cls, idx = int(m.group(6)), int(m.group(7))
            fmt = "NUM"
            if "CHORUS_LFO_RATE" in rest:
                fmt = "RATE_FIXED" if "Fixed" in rest else "RATE"
            elif "LFO_SHAPES" in rest:
                fmt = "SHAPE"
            elif "SPLIT_POINTS" in rest:
                fmt = "SPLIT"
            elif "FREQUENCIES" in rest:
                fmt = "FREQ"
            elif '"Manual"' in rest:
                fmt = "MANUAL"
            elif '"Wet"' in rest and '"-Wet"' in rest:
                fmt = "PM10"
            p = dict(idx=idx, label=short_label(m.group(5)), min=int(m.group(8)), max=int(m.group(9)), fmt=fmt,
                     table=-1)
        elif m.group(10) is not None:
            cls, idx = int(m.group(11)), int(m.group(12))
            var = params_var if m.group(13) == "params" else m.group(13)
            t = TABLES.index(var)
            p = dict(idx=idx, label=short_label(m.group(10)), min=0, max=len(SHORT[var]) - 1,
                     fmt="CHOICE", table=t)
        else:
            cls, idx = int(m.group(15)), int(m.group(16))
            p = dict(idx=idx, label=short_label(m.group(14)), min=0, max=1, fmt="CHECK", table=-1)
        assert cls == g, (cls, g, m.group(0))
        UI[g].append(p)

problems = []
for g in range(NGROUPS):
    got = sorted(p["idx"] for p in UI[g])
    if got != list(range(len(FX_INDICES[g]))):
        problems.append(f"group {g}: UI has {got}, FX_INDICES has {len(FX_INDICES[g])}")
    for p in UI[g]:
        if len(p["label"]) > 10:
            problems.append(f"group {g} param {p['idx']}: label '{p['label']}' > 10 chars")
if problems:
    print("\n".join(problems))
    if "--check" not in sys.argv:
        sys.exit(1)
if "--check" in sys.argv:
    print("max params per group:", max(len(UI[g]) for g in range(NGROUPS)))
    sys.exit(0)

# ---- pieces per UI parameter ----
for g in range(NGROUPS):
    for p in UI[g]:
        num = FX_INDICES[g][p["idx"]]
        p["pieces"] = [r for r in FX_PCL[g] if r[3] == num]
        assert p["pieces"], (g, p)

c = []
w = c.append
w("/* Generated by tools/ws_fx_gen.py from Edisyn (Apache 2.0): the Wavestation SR's effects. Do not edit. */")
w('#include "ws_fx.h"')
w("")


def cstr_list(name, items):
    w(f"const char *const {name}[{len(items)}] = {{")
    for i in range(0, len(items), 6):
        w("    " + ", ".join('"%s"' % s for s in items[i:i + 6]) + ",")
    w("};")
    w("")


cstr_list("ws_fx_type_names", TYPE_SHORT)
cstr_list("ws_fx_type_long", FX_NAMES)
w("const uint8_t ws_fx_type_group[WS_FX_TYPES] = {" + ", ".join(str(x) for x in FX_GROUP) + "};")
w("")
w("const uint8_t ws_fx_type_preset[WS_FX_TYPES][8] = {")
for t in range(56):
    w("    {" + ", ".join("0x%02X" % b for b in preset_block(t)) + "},  /* " + FX_NAMES[t] + " */")
w("};")
w("")
cstr_list("ws_fx_rates", CHORUS_RATE)
cstr_list("ws_fx_splits", SPLIT)
cstr_list("ws_fx_freqs", FREQ)
cstr_list("ws_fx_shapes", LFO_SHAPES)
for t, name in enumerate(TABLES):
    cstr_list("ws_fx_table_%d" % t, SHORT[name])
w("const char *const *const ws_fx_tables[%d] = {%s};" % (len(TABLES), ", ".join("ws_fx_table_%d" % t for t in range(len(TABLES)))))
w("")
for g in range(NGROUPS):
    for p in UI[g]:
        rows = ", ".join("{%d, %d, %d, %d}" % (r[0], r[1], r[2], r[4]) for r in p["pieces"])
        w(f"static const ws_fx_piece_t pc_{g}_{p['idx']}[] = {{{rows}}};")
w("")
for g in range(NGROUPS):
    if not UI[g]:
        continue
    w(f"static const ws_fx_param_t params_{g}[] = {{")
    for p in sorted(UI[g], key=lambda p: UI[g].index(p)):
        w('    {"%s", %d, %d, WS_FX_%s, %d, %d, pc_%d_%d},' % (
            p["label"], p["min"], p["max"], p["fmt"], p["table"], len(p["pieces"]), g, p["idx"]))
    w("};")
w("")
w("const ws_fx_group_t ws_fx_groups[WS_FX_GROUPS] = {")
for g in range(NGROUPS):
    if UI[g]:
        w("    {%d, params_%d}," % (len(UI[g]), g))
    else:
        w("    {0, 0},")
w("};")
w("")
w("/* Preset rows whose parameters Edisyn does not show (reserved) are in the presets above, kept as they are. */")
open(OUT, "w", encoding="utf-8").write("\n".join(c) + "\n")
print("wrote", OUT, "max params per group", max(len(UI[g]) for g in range(NGROUPS)))
