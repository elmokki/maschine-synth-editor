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
  - <Plugin>.pg          a zip with the GUI definition (purple.xml): knob
                         steps, number fields' display ranges and curves.
Copy config.xml and presets/ to reference/<Plugin>/ (gitignored) first.

Writes:
  - src/<plugin>_params.c/.h   parameter table (MIDI implementation facts only)
  - data/plugiator/<plugin>/NNN Name
        one file per factory preset, the raw MIDI bytes that set every
        in-preset parameter, channel nibble 0 (the firmware ORs in the
        real channel when sending). data/ is the LittleFS image source and
        is gitignored - factory presets are use-audio's data.

Run from the project root:
    python3 tools/plugiator_import.py MiniMax|B4000
"""
import collections
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT_SMALL_PX = 6      # font_small (Spleen 6x12) is monospaced, see tools/gen_font.py
NAME_MAX_PX = 59       # 64 px column minus margins, see draw_column in editor.c

# Values sent per position of a stepped control (PT_STEPS)
STEP_SETS = {
    "step6": [0, 22, 44, 66, 88, 110],         # MiniMax range/waveform knobs
    # B4000 drawbars, 0-8 (GUI MIDI monitor)
    "drawbar": [0, 15, 31, 47, 63, 79, 95, 111, 127],
    "octave": [0, 1, 2],                         # B4000 manual octaves
    # B4000 vibrato/chorus selector (GUI MIDI monitor: C-1 = 10, V-2 = 30,
    # C-2 = 50, V-3 = 70, C-3 = 90, V-1 = 120), in Hammond scanner order
    "vibrato": [120, 10, 30, 50, 70, 90],
}

# Position names (PT_STEPS / PT_LIST), PT_MIX ends, switch off/on names
LABEL_SETS = {
    "range": ["LO", "32'", "16'", "8'", "4'", "2'"],
    "wave": ["TRI", "SHARK", "SAW", "SQUARE", "WIDE", "NARROW"],
    # Delay note lengths in BPM mode, value = index (mapped with the
    # Plug-in Manager's MIDI monitor); P = dotted, T = triplet
    "note": ["1/1", "1/2P", "1/2", "1/2T", "1/4P", "1/4", "1/4T", "1/8P", "1/8", "1/8T",
             "1/16P", "1/16", "1/16T", "1/32P", "1/32", "1/32T", "1/64P", "1/64", "1/64T"],
    "chorus": ["CHORUS", "FLANGER"],
    "modmix": ["OSC3", "NOISE"],
    "octave": ["-1", "0", "+1"],
    "vibrato": ["V-1", "C-1", "V-2", "C-2", "V-3", "C-3"],
    "balance": ["BASS", "HORN"],
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

# The effects section is the same in every plug-in (MiniMax and B4000
# checked: same names, CCs and poly-AT ids)
EFFECTS = {
    "short": {
        "Chorus/Flanger": "FLANGER", "Rate": "CH RATE", "Depth": "CH DEPTH",
        "Ch. Dry/Wet": "CH MIX", "Chorus Dry/Wet": "CH MIX",
        "Phase": "CH PHASE", "Feedback@A74": "CH FDBK", "Feedback": "CH FDBK",
        "Time Left": "DLY T L", "Delay Feedback Left": "DLY FDBK",
        "Damp Left": "DAMP L", "Level Left": "LEVEL L", "Note Left": "NOTE L",
        "Note Right": "NOTE R", "Cross": "CROSS", "Time Right": "DLY T R",
        "Damp Right": "DAMP R", "Level Right": "LEVEL R",
        "BPM Left": "BPM L", "BPM Right": "BPM R", "Delay Dry/Wet": "DLY MIX",
        # "Bypass" in the GUI, but tested: 127 = effects on
        "BPM": "BPM", "Bypass": "FX ON", "Boost": "BOOST",
        "Volume Mic": "MIC VOL", "Volume@C7": "DEV VOL", "Volume": "DEV VOL",
    },
    "types": {
        "BPM Left": "switch", "BPM Right": "switch", "Cross": "switch",
        "Bypass": "switch", "Boost": "switch", "Chorus/Flanger": "switch",
        "Note Left": "list", "Note Right": "list", "BPM": "raw",
    },
    "labels": {
        "Note Left": "note", "Note Right": "note",
        "Chorus/Flanger": "chorus",  # a switch: off/on position names
    },
    # (low, high, decimals[, unit]) - see "display" below. GUI number fields
    # in purple.xml: delay times linear 1.01-1365.33 ms, tempo 72-199
    # (confirmed: value 0 = 72, 48 = 120), phase 0-180.
    "display": {
        "Phase": (0, 180, 0), "Feedback@A74": (-5, 5, 1), "Feedback": (-5, 5, 1),
        "BPM": (72, 199, 0),
        "Time Left": (1, 1365, 0, "ms"), "Time Right": (1, 1365, 0, "ms"),
    },
    # Logarithmic GUI number fields (purple.xml Curve_Type="Logarithmic"):
    # (low, high, intensity), value = low + (high-low) * (I^(v/127)-1)/(I-1).
    # Chorus rate confirmed against the GUI's own field at 11 points.
    "logscales": {
        "Rate": (0.01, 20.0, 1000),
    },
}

# Per plug-in hand-written knowledge the XML doesn't carry. Anything not
# listed falls back to the full name and a type guessed from the factory
# presets' values.
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
        },
        # Overrides where the guess from preset values would be wrong (e.g.
        # knobs that happen to sit at 0 in every factory preset)
        "types": {
            "Vcf Env Velocity": "cont", "Amp Env Velocity": "cont",
            "Volume@C11": "cont", "Bend Range": "raw24",
            "Osc1 Switch": "switch", "Osc2 Switch": "switch", "Osc3 Switch": "switch",
            "External Switch": "switch", "Noise Switch": "switch",
            "Modulation Mix": "mix",
        },
        "labels": {
            "Osc1 Range": "range", "Osc2 Range": "range", "Osc3 Range": "range",
            "Osc1 Waveform": "wave", "Osc2 Waveform": "wave", "Osc3 Waveform": "wave",
            "Modulation Mix": "modmix",  # a mix: 0 / 127 ends
        },
        "icons": {
            "Osc1 Waveform": "PI_WAVE", "Osc2 Waveform": "PI_WAVE", "Osc3 Waveform": "PI_WAVE",
        },
        # Display range of the plug-in GUI's knob scale, (low, high, decimals
        # [, unit]), mapped linearly onto 0-127; a range symmetric around 0
        # puts 0 at 64. Only for scales other than the GUI's usual 0-10 (those
        # stay 0-127) and only where the scale is linear (not the log time
        # knobs).
        "display": {
            "Tune": (-2, 2, 1), "Osc2 Frequency": (-10, 10, 1), "Osc3 Frequency": (-10, 10, 1),
            "Cutoff": (-4, 4, 1),
            "Pitch AT": (-4, 4, 1), "Vcf AT": (-4, 4, 1), "Amp AT": (-4, 4, 1),
        },
        # Envelope time knobs: the GUI has no number field for these, and its
        # panel legend turned out to be far off (e.g. "200 ms" at value 27 is
        # really ~26 ms). Curve = measured on the hardware instead, see CURVES.
        "curves": {
            "Vcf Env Attack": "env", "Vcf Env Decay": "env",
            "Amp Env Attack": "env", "Amp Env Decay": "env",
        },
    },
    "B4000": {
        "c_name": "b4000",
        "short": {
            "Mixer PEDAL": "PEDAL VOL", "Mixer LOWER": "LOWER VOL", "Mixer UPPER": "UPPER VOL",
            "Tone Bass": "BASS", "Swell": "SWELL", "Tone Treble": "TREBLE",
            "Pedal Octave": "PED OCT", "Lower Octave": "LOW OCT", "Upper Octave": "UPP OCT",
            "Lower Split Key": "LOW SPLIT", "Upper Split Key": "UPP SPLIT",
            "Pedal 16": "16'", "Pedal 5 1/3": "5 1/3'", "Pedal 8": "8'", "Pedal 4": "4'",
            "Pedal 2 2/3": "2 2/3'", "Pedal 2": "2'",
            "Lower 16": "16'", "Lower 5 1/3": "5 1/3'", "Lower 8": "8'", "Lower 4": "4'",
            "Lower 2 2/3": "2 2/3'", "Lower 2": "2'", "Lower 1 3/5": "1 3/5'",
            "Lower 1 113": "1 1/3'", "Lower 1": "1'",
            "Upper 16": "16'", "Upper 5 1/3": "5 1/3'", "Upper 8": "8'", "Upper 4": "4'",
            "Upper 2 2/3": "2 2/3'", "Upper 2": "2'", "Upper 1 3/5": "1 3/5'",
            "Upper 1 1/3": "1 1/3'", "Upper 1": "1'",
            "Perc Level": "PERC LVL", "Perc Decay": "PERC DEC", "Perc Harmonic": "PERC HARM",
            "Key Click": "KEY CLICK", "Drive": "DRIVE LVL", "Output Volume": "OUTPUT",
            "Vibrato Type": "VIB TYPE", "Vibrato Lower On/Off": "VIB LOWER",
            "Vibrato Upper On/Off": "VIB UPPER", "Rotor On/Off": "ROTOR",
            "Perc On/Off": "PERC", "Drive On/Off": "DRIVE", "Rotor Speed": "FAST",
            "MW/AT Select": "AT",
            "Horn Slow Speed": "H SLOW", "Horn Fast Speed": "H FAST",
            "Horn Acceleration": "H ACCEL", "Horn Brake": "H BRAKE", "Horn Tone": "H TONE",
            "Bass Slow Speed": "B SLOW", "Bass Fast Speed": "B FAST",
            "Bass Acceleration": "B ACCEL", "Bass Brake": "B BRAKE", "Bass Tone": "B TONE",
            "Spread": "SPREAD", "Balance": "BALANCE", "Distance": "DISTANCE",
            "External Input Level": "EXT LVL", "External On/Off": "EXT IN",
            "Condition": "CONDITION", "Tuning": "TUNING", "Leakage": "LEAKAGE",
            "Distortion": "DISTORT", "Attack": "ATTACK", "Release": "RELEASE",
            "Velocity On/Off": "VELOCITY",
        },
        # Drawbar names repeat per manual: C identifiers by manual instead
        "idents": {
            **{f"{m} {f}": f"{m.upper()}_{i}" for m in ("Pedal", "Lower", "Upper")
               for f, i in (("16", "16"), ("5 1/3", "5_1_3"), ("8", "8"), ("4", "4"),
                            ("2 2/3", "2_2_3"), ("2", "2"), ("1 3/5", "1_3_5"),
                            ("1 1/3", "1_1_3"), ("1", "1"))},
            "Lower 1 113": "LOWER_1_1_3",
        },
        "types": {
            **{f"{m} {f}": "steps:drawbar" for m in ("Pedal", "Lower", "Upper")
               for f in ("16", "5 1/3", "8", "4", "2 2/3", "2", "1 3/5", "1 1/3", "1")},
            "Lower 1 113": "steps:drawbar",
            "Pedal Octave": "steps:octave", "Lower Octave": "steps:octave",
            "Upper Octave": "steps:octave",
            "Lower Split Key": "note", "Upper Split Key": "note",
            # GUI MIDI monitor: CC73 is the 6-position selector, CC74 and
            # CC71 the lower/upper on/off buttons (see "swap" below)
            "Vibrato Type": "steps:vibrato",
            "Vibrato Lower On/Off": "switch", "Vibrato Upper On/Off": "switch",
            "Rotor Speed": "switch", "Rotor On/Off": "switch", "MW/AT Select": "switch",
            "External On/Off": "switch", "Velocity On/Off": "switch",
            "Perc On/Off": "switch", "Drive On/Off": "switch",
            "Balance": "mix",
        },
        "labels": {
            "Pedal Octave": "octave", "Lower Octave": "octave", "Upper Octave": "octave",
            "Vibrato Type": "vibrato", "Balance": "balance",
        },
        "icons": {
            **{f"{m} {f}": "PI_DRAWBAR_BROWN" for m in ("Pedal", "Lower", "Upper")
               for f in ("16", "5 1/3")},
            **{f"{m} {f}": "PI_DRAWBAR_WHITE" for m in ("Pedal", "Lower", "Upper")
               for f in ("8", "4", "2", "1")},
            **{f"{m} {f}": "PI_DRAWBAR_BLACK" for m in ("Pedal", "Lower", "Upper")
               for f in ("2 2/3", "1 3/5", "1 1/3")},
            "Lower 1 113": "PI_DRAWBAR_BLACK",
        },
        # Swell: the GUI knob is reversed (purple.xml Reverse="true") and
        # tested: CC11 = 0 is the loud end
        "reverse": {"Swell"},
        "display": {
            # Tone wheel tuning: "+/- one tone in 128 steps"
            "Tuning": (-2, 2, 1),
            # Rotor number fields in purple.xml
            "Horn Slow Speed": (0, 10, 1, "Hz"), "Bass Slow Speed": (0, 10, 1, "Hz"),
            "Horn Fast Speed": (0, 20, 1, "Hz"), "Bass Fast Speed": (0, 20, 1, "Hz"),
            "Horn Acceleration": (0, 30, 1, "s"), "Bass Acceleration": (0, 30, 1, "s"),
            "Horn Brake": (0, 30, 1, "s"), "Bass Brake": (0, 30, 1, "s"),
        },
        # The factory preset file has these two swapped (the Plug-in Manager
        # sends them as stored, so its own factory presets load wrongly):
        # C73 holds the on/off value and C74 the selector position.
        "swap": [("C73", "C74")],
    },
}

TYPE_ENUM = {"cont": "PT_CONT", "switch": "PT_SWITCH", "steps": "PT_STEPS",
             "raw24": "PT_RAW24", "raw": "PT_RAW", "list": "PT_LIST", "mix": "PT_MIX",
             "note": "PT_NOTE"}


def merged(spec, field):
    """Plug-in's own entries over the shared effects section's"""
    out = dict(EFFECTS.get(field, {}))
    out.update(spec.get(field, {}))
    return out


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
    shorts, types, labels = merged(spec, "short"), merged(spec, "types"), merged(spec, "labels")
    display, logscales = merged(spec, "display"), merged(spec, "logscales")
    cfg, presets = load(plugin)
    maps = cfg.find("midimap").findall("map")
    names_seen = collections.Counter(m.get("name") for m in maps)

    plist = [dict(p.attrib) for p in presets.iter("p")]
    for a, b in spec.get("swap", []):
        for p in plist:
            p[a], p[b] = p.get(b), p.get(a)
            for k in (a, b):
                if p[k] is None:
                    del p[k]
    observed = collections.defaultdict(collections.Counter)
    for p in plist:
        for k, v in p.items():
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
        ptype = types.get(key)
        if not ptype:
            vs = set(values)
            if vs and vs <= {0, 127} and "Button" in m.text:
                ptype = "switch"
            elif len(vs) >= 3 and vs <= set(STEP_SETS["step6"]):
                ptype = "steps:step6"
            else:
                ptype = "cont"
        steps = None
        if ptype.startswith("steps:"):
            ptype, steps = "steps", ptype.split(":")[1]
        short = shorts.get(key, name.upper())
        width = len(short) * FONT_SMALL_PX
        if width > NAME_MAX_PX:
            print(f"warning: short name {short!r} is {width:.0f} px wide (max {NAME_MAX_PX})")
        default = values.most_common(1)[0][0] if values else 0
        params.append({
            "name": name, "short": short, "addr": addr, "kind": addr[0],
            "num": int(addr[1:]), "type": ptype, "steps": steps, "default": default,
            "ident": spec.get("idents", {}).get(key),
            "in_preset": m.get("inpreset") == "yes",
            "page": 0 if m.text.startswith("pan1") else 1,
            "labels": labels.get(key),
            "icon": spec.get("icons", {}).get(key, "PI_NONE"),
            "reverse": key in spec.get("reverse", set()),
            "display": display.get(key),
            "curve": spec.get("curves", {}).get(key),
            "logscale": logscales.get(key),
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
    idents = set()
    for i, p in enumerate(params):
        ident = p["ident"] or re.sub(r"\W+", "_", p["short"]).strip("_").upper()
        if ident in idents:
            sys.exit(f"duplicate identifier {ident} ({p['name']}): add it to 'idents'")
        idents.add(ident)
        h.append(f"#define {upper}_{ident} {i}")
    c = [f"/* Generated by tools/plugiator_import.py from the Plug-in Manager's",
         f" * {plugin} config.xml - do not edit. */",
         f'#include "{c_name}_params.h"', ""]
    for name in sorted({p["curve"] for p in params if p["curve"]}):
        pts = CURVES[name]
        c.append(f"static const plug_curve_point_t curve_{name}_points[{len(pts)}] = {{"
                 + ", ".join(f"{{{v}, {ms}}}" for v, ms in pts) + "};")
        c.append(f"static const plug_curve_t curve_{name} = {{curve_{name}_points, {len(pts)}}};")
    for name in sorted({p["steps"] for p in params if p["steps"]}):
        vals = STEP_SETS[name]
        c.append(f"static const uint8_t steps_{name}[{len(vals)}] = {{"
                 + ", ".join(str(v) for v in vals) + "};")
    for name in sorted({p["labels"] for p in params if p["labels"]}):
        labels = LABEL_SETS[name]
        c.append(f"static const char *const labels_{name}[{len(labels)}] = {{"
                 + ", ".join(f'"{l}"' for l in labels) + "};")
    c += ["", f"const plug_param_t {c_name}_params[{upper}_PARAM_COUNT] = {{"]
    for p in params:
        short = p["short"].replace('"', '\\"')
        fields = [f'.short_name = "{short}"', f'.name = "{p["name"]}"',
                  f'.msg = PLUG_{"CC" if p["kind"] == "C" else "PAT"}', f'.number = {p["num"]}',
                  f'.type = {TYPE_ENUM[p["type"]]}', f'.default_value = {p["default"]}',
                  f'.in_preset = {"true" if p["in_preset"] else "false"}', f'.page = {p["page"]}']
        if p["labels"]:
            fields.append(f'.labels = labels_{p["labels"]}')
        if p["steps"]:
            fields += [f'.step_values = steps_{p["steps"]}',
                       f'.label_count = {len(STEP_SETS[p["steps"]])}']
        elif p["labels"]:
            fields.append(f'.label_count = {len(LABEL_SETS[p["labels"]])}')
        if p["icon"] != "PI_NONE":
            fields.append(f'.icon = {p["icon"]}')
        if p["reverse"]:
            fields.append(".reverse = true")
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
