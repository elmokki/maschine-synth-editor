# Maschine Mk2 → ESP32-S3 synth editor controller

Goal: drive a full-size Native Instruments Maschine Mk2 (the hardware controller) from an
ESP32-S3 over USB host, to use as a **programmer/editor controller for hardware synths**: the
Maschine's knobs, buttons and displays edit the synth over its DIN MIDI port (through the
Maschine's own USB-MIDI interface), and the ESP32 keeps a patch library on its flash.
The pads are a bonus (scales, chords).

## Status

Protocol fully reverse-engineered and confirmed on real hardware, directly over raw `hidraw`
(no USB packet capture needed at any point). See **[`docs/PROTOCOL.md`](docs/PROTOCOL.md)** for
the complete writeup: HID report layouts, the display/pad/button/encoder byte-level details,
every quirk and gotcha hit along the way (and why), and what's still unverified.

Critical path for the synth-editor use case:
- ✅ Encoders (main + 8 touch-encoders above the displays)
- ✅ Buttons (all 56 bits mapped; 48 confirmed by name)
- ✅ Displays (pixel-level control + text rendering via Pillow; one hardware display quirk and
  one unfixable ghosting limitation documented)
- ✅ 5-pin DIN MIDI — works directly via the device's standard USB-MIDI class interface, **no
  custom protocol needed at all** for this part (big simplification: no DIN jacks needed on the
  ESP32 board)
- ✅ Pads + pad LEDs (secondary priority, but needed first anyway to find/fix a few protocol-
  level bugs that also affected other things)

**ESP32-S3 firmware** (`src/`, PlatformIO + ESP-IDF USB host library): Maschine enumerates and
is powered from the board's OTG port; pads + pad LEDs, all buttons and encoders, both displays
(bitmap fonts) and DIN MIDI in/out all work on real hardware. A few full-speed-only quirks had to
be worked around — see **[`docs/ESP32_NOTES.md`](docs/ESP32_NOTES.md)**.

## Synths

Group buttons A-H pick a section (functions of the synth), the main encoder pages through it, the eight
encoders and display buttons above the displays edit the values. Every editor has a patch library on
LittleFS (browse, store, categories), a patch overview (Grid button), envelope/vector pictures, and a
tutorial for the synth's special controls (Shift + Control). Reading back from the unit needs its MIDI OUT
on the Maschine's MIDI IN (see below for what needs it); editing and playing never do.

**Name lists (Note Repeat):** fills both displays with names in small text, 24 a page. With Browse off and a
wave knob on the page (Wavestation PATCH and SEQ) it lists the waves; otherwise, or with Browse on, the library's
patches. The main encoder moves the cursor (Shift: a page), Enter picks (loads the patch / sets the wave) and
closes the list, Browse off closes the patch list, a Group button goes back to the editor. In a name list
the knob of the value itself (WAVE, TONE, ...) moves the cursor too. Lists exist for: Wavestation PATCH / SEQ waves,
Wavestation PERF patches, JV-880 waves (PATCH, RHYTHM) and performance-part patches, D-110 PCM waves (TONE),
tones (PATCH, RHYTHM key) and U-110 tones.

| Synth | Editor | State |
|---|---|---|
| Plugiator MiniMax, B4000 (organ) | all parameters, factory presets | done |
| Volca FM2 | DX7 voices (all operators, algorithm picture), `.syx` import | done |
| Roland D-110 | TONE, PATCH, RHYTHM; read from the unit, write to the unit | done |
| Roland JV-880 | PATCH (4 tones), RHYTHM (61 keys), PERF (8 parts); expansion boards, read, IMPORT, store to the unit | done |
| Roland U-110 | one patch = 6 parts, read, IMPORT, store to the unit | done |
| Korg Wavestation SR | PATCH (4 waves, vector mix, SysEx parameters); IMPORT of RAM/ROM/card banks and store into RAM banks (bank dumps) | done |
| Korg Wavestation SR | PERF (8 parts: patch, level, bus, ranges, tuning, play settings; effects: routing, mixes, both effect units with all 55 types); IMPORT, read, store | done |
| Korg Wavestation SR | SEQ (wave sequences: up to 255 steps with wave, level, duration, crossfade, tuning; start, loop; modulation amount is not written); IMPORT of the banks' sequences, read, Grid overview, listening | done: REC > TO SR writes into any RAM 1-3 sequence (see the warning below) |

### What needs the MIDI IN cable

The unit's MIDI OUT goes to the Maschine's MIDI IN only for reading back:

| Editor | Needs the cable for | Works without it |
|---|---|---|
| Plugiator MiniMax, B4000; Volca FM2 | nothing | everything |
| Roland D-110, JV-880, U-110 (all entries) | Auto (read the sound from the unit), IMPORT | editing, playing, **storing into the unit** (direct writes); the remembered patch is loaded when the unit does not answer |
| Korg Wavestation SR PATCH, PERF, SEQ | Auto, IMPORT, and **storing into the SR's memory (REC > TO SR, on all three entries)**: the SR only takes whole banks, so the bank is read first, one item replaced and the bank sent back | editing and sending to the edit buffer (PATCH, PERF), playing, listening to a SEQ sequence, saving to the library |
| Sequencer (Scene) | note entry from a keyboard on the MIDI IN | everything else |

In short: without the cable the Roland editors (and the Plugiator and Volca ones) only lose the knowledge of what is in
the unit; storing into it still works. The Wavestation PATCH and PERF entries are live editors (the sound goes to the
SR's edit buffer), but cannot store into the SR or read from it; SEQ then only edits the library, because a sequence
has no edit buffer to hear or keep an edit in.

Also in: pad scales and chords, optional pad aftertouch (Pad Mode: OFF, channel or polyphonic; only a hard press, raw 3000-4000, counts), MIDI thru, LED brightness, a Volume button for the synths that have one.

## Sequencer

The **Scene** button opens a step sequencer that runs on top of every synth (it keeps playing when you leave
the screen; **Play** starts and stops it from anywhere, and Scene pulses while it plays).

- 8 tracks (Group A-H, each its own colour), up to 128 steps each, 16th notes; every track has its own
  length, MIDI channel, transpose and mute. A step has a note, velocity, gate, ratchet (1-8 hits) and a tie
  (continues the previous note; a different note is played legato).
- The 16 pads are the shown page's steps; Step arrows turn the page. **Rec** switches between WRITE (pads
  switch steps on and off) and SELECT (pads only pick the step to edit). The NOTE knob moves along the scale
  chosen on the Pad Mode screen, and a new step starts from the previous step's note or the scale's root.
- **Control** inside Scene opens the sequencer's settings: a MIDI IN channel on which incoming notes fill the
  picked step (pitch, velocity, and how long the key is held becomes the gate or a chain of ties).
- **Pattern** opens 16 pattern slots on the pads, stored on LittleFS and saved automatically; a playing
  sequencer switches at the end of the longest track. Erase + a pad clears a slot, Erase + a Group button
  clears a track.
- The engine runs in its own task on core 1, clocked by a hardware timer.
- **Shift + Control** in Scene opens the sequencer's own tutorial.

Not yet: MIDI clock out, live recording, chords (polyphony), chaining patterns into scenes.

> **Warning, Wavestation SR SEQ:** the SR has no edit buffer for wave sequences, so REC > TO SR replaces the chosen
> sequence in the unit for good. Any sequence of RAM 1-3 can be chosen (encoder 8 on the Store screen, which starts at
> **RAM 3, sequence 31**, the last one; the slot's name as of the last IMPORT is shown). The whole bank is kept as it
> was in `/lfs/ws/backup/seq00` - `seq02` first (one file a bank, overwritten by the next write to that bank).

### Planned

- Jump by letter in the name lists, Wavestation SR SEQ copy / paste of steps.
- Sequencer: MIDI clock out, live recording (Shift + Rec), chords, pattern chaining.
- Korg NS5R (documents are in the local `reference/`).
- Volca Drum.
- Overviews for the RHYTHM entries (JV-880, D-110).
- Scene: saved multi-synth scenes (patterns of the sequencer plus each synth's patch).
- A MIDI joystick of the author's own (vector control for the Wavestation, CC 16/17 through a splitter).

## Layout

- `docs/PROTOCOL.md` — the protocol reference. Read this first.
- `docs/ESP32_NOTES.md` — firmware notes, step by step, incl. the full-speed workarounds.
- `src/` — ESP32-S3 firmware (`main.c` HID side, `display.c`, `midi.c`, generated `fonts.c`, `editor.c` the generic
  editor, `synth_*.c` one driver per synth entry, `sequencer.c` / `seq_ui.c` the step sequencer; `CLAUDE.md` has the code map).
- `docs/WAVESTATION.md` — notes on the Wavestation SR's SysEx and data.
- `tools/ws_fx_gen.py` — generates `src/ws_fx_data.c` (the Wavestation effect tables) from the local Edisyn clone.
- `tools/gen_font.py` — regenerates `src/fonts.c`/`fonts.h` from a TTF.
- `tools/*_import.py`, `tools/*_names.py` — build the patch libraries and name lists in `data/` for each synth.
- `tools/plugiator_import.py` — Plugiator parameter tables + factory presets from the Plug-in
  Manager's files (see `docs/ESP32_NOTES.md`, Step 5).
- `reference/`, `data/` — local only (gitignored): third-party manuals and factory presets.
- `pc_reference/` — small self-contained Python scripts (not copied from any third-party
  project) used to find and verify the protocol, and to keep verifying it quickly without
  needing to flash firmware. Point them at your Maschine Mk2's `/dev/hidrawN` node.

## Why PC-first

Everything was prototyped and verified on a PC first (`pc_reference/`) because iteration there
is seconds, not a flash-and-reboot cycle. The plan is to port the now-confirmed protocol
knowledge in `docs/PROTOCOL.md` directly to ESP32-S3 firmware (ESP-IDF's native USB host
library: a separate, simple USB-MIDI code path for the DIN MIDI side, and a HID code path for
everything else), rather than re-discovering any of this on-device.

## Credit / prior art

No third-party code is vendored in this repo. While reverse-engineering, these existing
open-source projects were read (not copied) for cross-reference, and are credited in detail in
`docs/PROTOCOL.md` along with exactly which of their assumptions turned out to be wrong for this
unit and had to be re-verified:
- [`shaduzlabs/cabl`](https://github.com/shaduzlabs/cabl) (MIT)
- [`SnovaxZ/MaschineMK2_linux`](https://github.com/SnovaxZ/MaschineMK2_linux) (GPL-3.0)

## Third-party material (not in this repository)

`reference/` (manuals, factory banks, notes) and `data/` (the LittleFS image: patch libraries, name lists) are
local only. The `tools/*_import.py` and `*_names.py` scripts build `data/` from them; they expect
`reference/edisyn/` (a clone of [eclab/edisyn](https://github.com/eclab/edisyn), Apache 2.0, whose parameter
tables and SysEx layouts several drivers follow) and the synths' own manuals and patch lists, which you have to
supply yourself. Fonts in `tools/fonts/` are Spleen (BSD-2, see `tools/fonts/LICENSE.spleen`).

Licence: MIT (see `LICENSE`).
