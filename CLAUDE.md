# Mascine_MK2

ESP32-S3 (N16R8) firmware that drives a full-size Native Instruments Maschine Mk2 over USB host
and turns it into an editor/librarian for hardware synths. Hardware protocol: `docs/PROTOCOL.md`;
firmware notes: `docs/ESP32_NOTES.md`. The user writes in Finnish; the UI and code are English.

## Build / flash
- PlatformIO + ESP-IDF 5.5: `~/.platformio/penv/bin/pio run` (build), `... run -t upload --upload-port /dev/ttyACM0`.
- LittleFS (`storage` partition, mounted at /lfs) is built from `data/`. `pio run -t uploadfs` **replaces
  it and wipes the user's patches and settings**; to add files keep the rest with
  `~/.platformio/penv/bin/python tools/lfs_merge.py <data/ subdirs> --write` (reads the partition back first).
- Serial log: 115200 on /dev/ttyACM0. Kill a logger with `pkill -f "^/home/elmokki/.platformio/penv/bin/python .*serlog.py"`
  (a looser pattern kills the calling shell).
- `reference/` (manuals, factory banks, notes) and `data/` are gitignored: third-party material, never commit it.

## Code map
- `src/main.c`: USB host, HID (buttons, encoders, pads, LEDs, displays), LED gain. `src/midi.c`: DIN MIDI via the
  Maschine's USB-MIDI interface (SysEx out/in, thru, note-in handler).
- `src/editor.c`: the generic editor (pages/sections, patches, store/name, menus, pads, tutorial).
  `src/synth.h`: the driver interface - one driver per synth entry (`synths[]` in editor.c).
- Drivers: `synth_minimax.c`, `synth_b4000.c` (+ `plugiator.c`), `synth_volca.c` (DX7 voices),
  `synth_d110.c` (TONE), `synth_d110p.c` (PATCH), `synth_d110r.c` (RHYTHM), `d110_common.c`,
  `synth_jv880.c` (Patch mode; SysEx helpers in `jv880_common.c`).
- Importers in `tools/`: `plugiator_import.py`, `dx7_import.py`, `d110_import.py`, `jv880_import.py` (Init patch),
  `jv880_names.py` (waveform names from `reference/JV880/` lists -> `data/jv880/waves/`; never reads the `.bin` images).

## Working with the user (agreed rules)
- Plan UI/layout changes with the user before implementing; small liberties are fine if listed afterwards.
- Layouts follow the synth's own panel/programmer logic; switches sit above their knob; parallel sections keep the page.
- Group buttons are always sections. Shift combos need a stated reason. Nothing in the pads area
  (pads + the Scene...Mute column) is synth-specific. The editor must work without a MIDI IN cable
  (reading back is optional).
- Commit only when asked; end commit messages with the Co-Authored-By line given in the session.
- Hardware tests: the user may be away - use open-ended background captures, never short timers.

## Gotchas
- LED reports go out before display chunks: never set an LED to two values in one update, and rate-limit
  animations (~50 ms), or the displays stop updating.
- Roland addresses are 7-bit: write base addresses as their bytes (04 00 00) and add offsets in 7-bit
  arithmetic; verify against the manual's examples before sending (a bug once wrote into the D-110 System Area).
- Note names: 60 = C4 everywhere.

## State (2026-10-05)
Done: Plugiator MiniMax/B4000, Volca FM2, D-110 (TONE, PATCH, RHYTHM: import, read, write to unit, volume),
categories, pad scales/chords, MIDI thru, LED brightness, tutorial (Shift + Control), JV-880 PATCH (all params,
Group = function block, tones via Step arrows / Shift + Group A-D, read, write, store to I01-I64), Rec + Erase
reverts a sound read from the unit (all D-110 and JV-880 drivers).
JV-880 expansion boards: Setup CARD (0-22; 20-22 = boards 97-99) picks a library (`jv880/cardNN`; `jv880/` itself is
shown with every board); IMPORT reads Internal as the board's 001-064, as own patches (200+) or the presets A+B
(065-192). Wave names from `data/jv880/waves/` (int, cardNN). Unit settings needed: Unit # 17, Exclusive Rx ON, patch
Rx channel = PATCH CH, Program change / Bank select Rx ON for the preset import, JV MIDI OUT -> Maschine MIDI IN to read.
Editor feel (all synths): held arrows repeat (faster after 2 s), Shift + main encoder browses 8 patches a detent,
wide-range parameter knobs accelerate when turned fast (`accelerated()` in editor.c), the header gives the patch name
the room while browsing. JV-880 PATCH is done; the user tests on the real unit.
Next: JV-880 PERF and RHYTHM entries (plan with the user first; three entries agreed, like the D-110). Boards 19 (House) and 97
(Experience III) have no waveform list in `reference/JV880/` - left as they are on purpose (names show as numbers).
Ideas waiting: synth list as per-device card stacks; Scene = saved multi-synth scenes; Volca Drum.
