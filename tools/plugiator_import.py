#!/usr/bin/env python3
"""Import a Plugiator plug-in's parameter map and factory presets from the
use-audio Plug-in Manager's own files.

The Plug-in Manager (a XULRunner app) keeps, per plug-in, under
  Program Files (x86)/Use Audio/Plug-in Manager/chrome/devices/<Plugin>/:
  - config.xml           <map name="Cutoff" midi="C74" inpreset="yes">pan1_ID_CutOffKnob</map>
                         C<n> = Control Change n, A<n> = Polyphonic Aftertouch
                         with note number n used as a parameter number.
  - presets/default.xml  <p n="Lucky Man" g="Leads" o="0" C74="99" A75="0" .../>
                         i.e. each preset stores the raw MIDI values directly.

Writes:
  - src/<plugin>_params.c/.h   parameter table (MIDI implementation facts only)
  - data/plugiator/<plugin>/NNN Name
        one file per factory preset, the raw MIDI bytes that set every
        in-preset parameter, channel nibble 0 (the firmware ORs in the
        real channel when sending). data/ is the LittleFS image source and
        is gitignored - factory presets are use-audio's data.

Run from the project root:
    python3 tools/plugiator_import.py MiniMax
"""
import collections
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT_SMALL_PX = 6      # font_small (Spleen 6x12) is monospaced, see tools/gen_font.py
NAME_MAX_PX = 59       # 64 px column minus margins, see draw_column in editor.c

STEP6 = [0, 22, 44, 66, 88, 110]

# Per plug-in hand-written knowledge the XML doesn't carry: short display
# names and parameter types. Anything not listed falls back to the full name
# and a type guessed from the factory presets' values.
PLUGINS = {
    "MiniMax": {
        "c_name": "minimax",
        "short": {
            "Tune": "TUNE", "Glide Time": "GLIDE", "Modulation Mix": "MOD MIX",
            "Glide Switch": "GLIDE SW", "Env Decay": "DECAY SW",
            "Osc Modulation": "OSC MOD",
            "Osc1 Range": "OSC1 RNG", "Osc1 Waveform": "OSC1 WAV",
            "Osc2 Range": "OSC2 RNG", "Osc2 Waveform": "OSC2 WAV",
            "Osc3 Range": "OSC3 RNG", "Osc3 Waveform": "OSC3 WAV",
            "Osc2 Frequency": "OSC2 FRQ", "Osc3 Frequency": "OSC3 FRQ",
            "Osc3 Control": "OSC3 CTL",
            "Osc1 Volume": "OSC1 VOL", "Osc2 Volume": "OSC2 VOL", "Osc3 Volume": "OSC3 VOL",
            # Tested: "External Switch" turns the output-to-mixer feedback
            # on (the GUI's Feedback Level knob is "External Volume"), and
            # the separate "Feedback" switch (C38) does nothing audible
            "Osc1 Switch": "OSC1", "External Switch": "FEEDBACK", "Osc2 Switch": "OSC2",
            "Noise Switch": "NOISE", "Osc3 Switch": "OSC3",
            "External Volume": "FDBK LVL", "Noise Volume": "NOISE VOL",
            "Feedback@C38": "FDBK C38", "Noise Type": "PINK",
            "Vcf Modulation": "FILT MOD", "Vcf Kybd Ctrl1": "KBD 1", "Vcf Kybd Ctrl2": "KBD 2",
            "Cutoff": "CUTOFF", "Emphasis": "EMPHASIS", "Amount of Contour": "CONTOUR",
            "Vcf Env Attack": "F ATTACK", "Vcf Env Decay": "F DECAY", "Vcf Env Sustain": "F SUSTAIN",
            "Amp Env Attack": "A ATTACK", "Amp Env Decay": "A DECAY", "Amp Env Sustain": "A SUSTAIN",
            "Volume@C11": "VOLUME", "Vcf Env Velocity": "F VELO", "Amp Env Velocity": "A VELO",
            "Bend Range": "BEND", "MW Intensity": "MW INT", "MW Offset": "MW OFFS",
            "Low Note Priority": "LO NOTE", "Env Retrig": "RETRIG", "Single Mode": "SINGLE",
            "Modulation": "MOD WHL", "Pitch AT": "CV OSC", "Vcf AT": "CV FILT", "Amp AT": "CV LOUD",
            "Chorus/Flanger": "FLANGER", "Rate": "CH RATE", "Depth": "CH DEPTH",
            "Ch. Dry/Wet": "CH MIX", "Phase": "CH PHASE", "Feedback@A74": "CH FDBK",
            "Time Left": "DLY T L", "Delay Feedback Left": "DLY FDBK",
            "Damp Left": "DAMP L", "Level Left": "LEVEL L", "Note Left": "NOTE L",
            "Note Right": "NOTE R", "Cross": "CROSS", "Time Right": "DLY T R",
            "Damp Right": "DAMP R", "Level Right": "LEVEL R",
            "BPM Left": "BPM L", "BPM Right": "BPM R", "Delay Dry/Wet": "DLY MIX",
            "BPM": "BPM", "Bypass": "FX ON", "Boost": "BOOST",
            "Volume Mic": "MIC VOL", "Volume@C7": "DEV VOL",
        },
        # Overrides where the guess from preset values would be wrong (e.g.
        # knobs that happen to sit at 0 in every factory preset)
        "types": {
            "Vcf Env Velocity": "cont", "Amp Env Velocity": "cont",
            "Volume@C11": "cont", "Bend Range": "raw24",
            "Osc1 Switch": "switch", "Osc2 Switch": "switch", "Osc3 Switch": "switch",
            "External Switch": "switch", "Noise Switch": "switch",
            "BPM Left": "switch", "BPM Right": "switch", "Cross": "switch",
            "Bypass": "switch", "Boost": "switch", "Chorus/Flanger": "switch",
            "Note Left": "list", "Note Right": "list", "BPM": "raw",
            "Modulation Mix": "mix",
        },
        "labels": {
            "Osc1 Range": "range", "Osc2 Range": "range", "Osc3 Range": "range",
            "Osc1 Waveform": "wave", "Osc2 Waveform": "wave", "Osc3 Waveform": "wave",
            "Note Left": "note", "Note Right": "note",
            "Chorus/Flanger": "chorus",  # a switch: off/on position names
            "Modulation Mix": "modmix",  # a mix: 0 / 127 ends
        },
        # Display range of the plug-in GUI's knob scale, (low, high, decimals
        # [, unit]), mapped linearly onto 0-127; a range symmetric around 0
        # puts 0 at 64. Only for scales other than the GUI's usual 0-10 (those
        # stay 0-127) and only where the scale is linear (not the log time
        # knobs). The delay times are the GUI's own number fields (purple.xml
        # in Minimax.pg: linear, 1.01-1365.33 ms).
        "display": {
            "Tune": (-2, 2, 1), "Osc2 Frequency": (-10, 10, 1), "Osc3 Frequency": (-10, 10, 1),
            "Cutoff": (-4, 4, 1),
            "Pitch AT": (-4, 4, 1), "Vcf AT": (-4, 4, 1), "Amp AT": (-4, 4, 1),
            "Phase": (0, 180, 0), "Feedback@A74": (-5, 5, 1),
            "BPM": (72, 199, 0),  # confirmed: value 0 = 72 BPM, 48 = 120 BPM
            "Time Left": (1, 1365, 0, "ms"), "Time Right": (1, 1365, 0, "ms"),
        },
        # Envelope time knobs: the GUI has no number field for these, and its
        # panel legend turned out to be far off (e.g. "200 ms" at value 27 is
        # really ~26 ms). Curve = measured on the hardware instead, see CURVES.
        # Logarithmic GUI number fields (purple.xml Curve_Type="Logarithmic"):
        # (low, high, intensity), value = low + (high-low) * (I^(v/127)-1)/(I-1).
        # Chorus rate confirmed against the GUI's own field at 11 points.
        "logscales": {
            "Rate": (0.01, 20.0, 1000),
        },
        "curves": {
            "Vcf Env Attack": "env", "Vcf Env Decay": "env",
            "Amp Env Attack": "env", "Amp Env Decay": "env",
        },
    },
}

# Minimoog panel legends for the 6-position knobs
LABEL_SETS = {
    "range": ["LO", "32'", "16'", "8'", "4'", "2'"],
    "wave": ["TRI", "SHARK", "SAW", "SQUARE", "WIDE", "NARROW"],
    # Delay note lengths in BPM mode, value = index (mapped with the
    # Plug-in Manager's MIDI monitor); P = dotted, T = triplet
    "note": ["1/1", "1/2P", "1/2", "1/2T", "1/4P", "1/4", "1/4T", "1/8P", "1/8", "1/8T",
             "1/16P", "1/16", "1/16T", "1/32P", "1/32", "1/32T", "1/64P", "1/64", "1/64T"],
    "chorus": ["CHORUS", "FLANGER"],
    "modmix": ["OSC3", "NOISE"],
}

# (MIDI value, milliseconds) points of a time knob, interpolated
# logarithmically in between.
# "env": measured 2026-10-04 on the Plugiator (USB MIDI from the PC, audio
# recorded through a Scarlett): amp attack time from the start of the rise
# to full level, sustain 127. Decay uses the same time scale (its time to
# 50 % matches the attack time at every value; to near silence it takes
# ~3.5x longer). The filter envelope's knobs carry the same panel legend
# and are assumed to match.
CURVES = {
    "env": [(0, 1), (7, 7), (14, 12), (20, 16), (27, 26), (34, 63), (41, 92), (51, 171),
            (60, 328), (67, 619), (74, 1095), (82, 1938), (91, 3141), (99, 4501),
            (107, 5835), (117, 7828), (127, 10710)],
}

TYPE_ENUM = {"cont": "PT_CONT", "switch": "PT_SWITCH", "step6": "PT_STEP6",
             "raw24": "PT_RAW24", "raw": "PT_RAW", "list": "PT_LIST", "mix": "PT_MIX"}


def load(plugin):
    cfg = ET.parse(os.path.join(ROOT, "reference", plugin, "config.xml")).getroot()
    presets = ET.parse(os.path.join(ROOT, "reference", plugin, "presets", "default.xml")).getroot()
    return cfg, presets


def key_for(name, addr, names_seen):
    # Some names repeat ("Feedback", "Volume"); disambiguate those by address
    return f"{name}@{addr}" if names_seen[name] > 1 else name


def main():
    plugin = sys.argv[1] if len(sys.argv) > 1 else "MiniMax"
    spec = PLUGINS[plugin]
    cfg, presets = load(plugin)
    maps = cfg.find("midimap").findall("map")
    names_seen = collections.Counter(m.get("name") for m in maps)

    observed = collections.defaultdict(collections.Counter)
    plist = list(presets.iter("p"))
    for p in plist:
        for k, v in p.attrib.items():
            if re.fullmatch(r"[CA]\d+", k):
                observed[k][int(v)] += 1

    params, addrs_done = [], set()
    for m in maps:
        name, addr = m.get("name"), m.get("midi")
        if addr in addrs_done:
            # config.xml maps both delay feedbacks to C119; keep the first
            print(f"note: {name} shares {addr} with an earlier parameter, skipped")
            continue
        addrs_done.add(addr)
        key = key_for(name, addr, names_seen)
        values = observed.get(addr, collections.Counter())
        ptype = spec["types"].get(key)
        if not ptype:
            vs = set(values)
            if vs and vs <= {0, 127} and "Button" in m.text:
                ptype = "switch"
            elif len(vs) >= 3 and vs <= set(STEP6):
                ptype = "step6"
            else:
                ptype = "cont"
        short = spec["short"].get(key, name.upper())
        width = len(short) * FONT_SMALL_PX
        if width > NAME_MAX_PX:
            print(f"warning: short name {short!r} is {width:.0f} px wide (max {NAME_MAX_PX})")
        default = values.most_common(1)[0][0] if values else 0
        params.append({
            "name": name, "short": short, "addr": addr, "kind": addr[0],
            "num": int(addr[1:]), "type": ptype, "default": default,
            "in_preset": m.get("inpreset") == "yes",
            "page": 0 if m.text.startswith("pan1") else 1,
            "labels": spec["labels"].get(key),
            "icon": "PI_WAVE" if spec["labels"].get(key) == "wave" else "PI_NONE",
            "display": spec.get("display", {}).get(key),
            "curve": spec.get("curves", {}).get(key),
            "logscale": spec.get("logscales", {}).get(key),
        })

    write_c(plugin, spec["c_name"], params)
    write_presets(plugin, spec["c_name"], params, plist)

    for p in params:
        print(f"  {p['addr']:5} {p['type']:6} def {p['default']:3}  {p['short']:10} {p['name']}")


def write_c(plugin, c_name, params):
    upper = c_name.upper()
    h = [f"/* Generated by tools/plugiator_import.py from the Plug-in Manager's",
         f" * {plugin} config.xml - do not edit. */",
         "#pragma once", '#include "plugiator.h"', "",
         f"#define {upper}_PARAM_COUNT {len(params)}",
         f"extern const plug_param_t {c_name}_params[{upper}_PARAM_COUNT];", ""]
    for i, p in enumerate(params):
        ident = re.sub(r"\W+", "_", p["short"]).strip("_").upper()
        h.append(f"#define {upper}_{ident} {i}")
    c = [f"/* Generated by tools/plugiator_import.py from the Plug-in Manager's",
         f" * {plugin} config.xml - do not edit. */",
         f'#include "{c_name}_params.h"', ""]
    for name in sorted({p["curve"] for p in params if p["curve"]}):
        pts = CURVES[name]
        c.append(f"static const plug_curve_point_t curve_{name}_points[{len(pts)}] = {{"
                 + ", ".join(f"{{{v}, {ms}}}" for v, ms in pts) + "};")
        c.append(f"static const plug_curve_t curve_{name} = {{curve_{name}_points, {len(pts)}}};")
    used = {p["labels"] for p in params if p["labels"]}
    for name, labels in LABEL_SETS.items():
        if name in used:
            c.append(f"static const char *const labels_{name}[{len(labels)}] = {{"
                     + ", ".join(f'"{l}"' for l in labels) + "};")
    c += ["", f"const plug_param_t {c_name}_params[{upper}_PARAM_COUNT] = {{"]
    for p in params:
        fields = [f'.short_name = "{p["short"]}"', f'.name = "{p["name"]}"',
                  f'.msg = PLUG_{"CC" if p["kind"] == "C" else "PAT"}', f'.number = {p["num"]}',
                  f'.type = {TYPE_ENUM[p["type"]]}', f'.default_value = {p["default"]}',
                  f'.in_preset = {"true" if p["in_preset"] else "false"}', f'.page = {p["page"]}']
        if p["labels"]:
            fields += [f'.labels = labels_{p["labels"]}', f'.label_count = {len(LABEL_SETS[p["labels"]])}']
        if p["icon"] != "PI_NONE":
            fields.append(f'.icon = {p["icon"]}')
        if p["display"]:
            lo, hi, dec = p["display"][:3]
            fields += [f".disp_lo = {lo}", f".disp_hi = {hi}", f".decimals = {dec}"]
            if len(p["display"]) > 3:
                fields.append(f'.unit = "{p["display"][3]}"')
        if p["curve"]:
            fields.append(f'.time_curve = &curve_{p["curve"]}')
        if p["logscale"]:
            lo, hi, k = p["logscale"]
            fields += [f".log_lo = {lo}f", f".log_hi = {hi}f", f".log_k = {k}"]
        c.append("    {" + ", ".join(fields) + "},")
    c.append("};")
    open(os.path.join(ROOT, "src", f"{c_name}_params.h"), "w").write("\n".join(h) + "\n")
    open(os.path.join(ROOT, "src", f"{c_name}_params.c"), "w").write("\n".join(c) + "\n")


def write_presets(plugin, c_name, params, plist):
    out_dir = os.path.join(ROOT, "data", "plugiator", c_name)
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, f))
    missing = collections.Counter()
    for p in plist:
        data = bytearray()
        for prm in params:
            if not prm["in_preset"]:
                continue
            v = p.get(prm["addr"])
            if v is None:
                missing[prm["addr"]] += 1
                v = prm["default"]
            data += bytes([0xB0 if prm["kind"] == "C" else 0xA0, prm["num"], int(v) & 0x7F])
        name = re.sub(r'[\\/:*?"<>|]', "_", p.get("n", "").strip()) or "Untitled"
        fname = f"{int(p.get('o', 0)):03d} {name}"[:63]  # LittleFS name limit 64
        open(os.path.join(out_dir, fname), "wb").write(data)
    print(f"{len(plist)} presets -> {os.path.relpath(out_dir, ROOT)}")
    if missing:
        print("filled from defaults (param: presets missing it):",
              ", ".join(f"{k}: {v}" for k, v in sorted(missing.items())))


if __name__ == "__main__":
    main()
