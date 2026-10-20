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
  `synth_jv880.c` (Patch mode), `synth_jv880r.c` (RHYTHM: Rhythm Setup, 61 keys x 52 bytes, Performance mode),
  `synth_jv880p.c` (PERF: performance = common + 8 parts, 311 bytes);
  `jv880_common.c`: SysEx helpers, board/wave names, envelope drawing.
  `synth_u110.c` (Roland U-110: patch = 6 parts, individual-parameter DT1s only so far).
- `synth_wsp.c` (Korg Wavestation SR PATCH: 4 waves, vector mix, SysEx parameter changes as ASCII text, pictures via page_draw/column_draw).
- Importers in `tools/`: `plugiator_import.py`, `dx7_import.py`, `d110_import.py`, `jv880_import.py` (Init patch), `jv880r_import.py` (Init kit), `jv880p_import.py` (Init performance),
  `jv880_names.py` (waveform names from `reference/JV880/` lists -> `data/jv880/waves/`; never reads the `.bin` images),
  `ws_import.py` (Wavestation names + Init patch from the cloned Edisyn), `u110_names.py` (tone names from the cloned `reference/edisyn/` -> `data/u110/tones/`), `u110_import.py` (Init patch).

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
JV-880 layout rule (user): the waveform/part choice is Group A (PATCH: A WAVE, B COMMON, C PITCH ...); pages are full (8 knobs + 8 buttons), few sections.
JV-880 expansion boards: Setup CARD (0-22; 20-22 = boards 97-99) picks a library (`jv880/cardNN`; `jv880/` itself is
shown with every board); IMPORT reads Internal as the board's 001-064, as own patches (200+) or the presets A+B
(065-192). Wave names from `data/jv880/waves/` (int, cardNN). Unit settings needed: Unit # 17, Exclusive Rx ON, patch
Rx channel = PATCH CH, Program change / Bank select Rx ON for the preset import, JV MIDI OUT -> Maschine MIDI IN to read.
Editor feel (all synths): held arrows repeat (faster after 2 s), Shift + main encoder browses 8 patches a detent,
wide-range parameter knobs accelerate when turned fast (`accelerated()` in editor.c), the header gives the patch name
the room while browsing. JV-880 PATCH is done; the user tests on the real unit.
JV-880 RHYTHM (2026-10-06, committed; editing tested, write to the unit not): Group A WAVE, B PITCH ENV, C TVF, D TVA, E OUTPUT, F KIT; sets Performance mode, reads part 8's Rx channel.
JV-880 PERF (2026-10-06, committed; read, Setup IMPORT and the patch names tested, write to I01-I16 not): A PART (all of a part's settings on one page), B COMMON (reverb, chorus); part 1-8 = Shift + Group A-H or Step arrows; Internal patch and performance names read with Auto; Setup IMPORT saves I01-I16 as library 001-016 (own from 100); REC > TO JV-880 shows the slot's name. Part 8's Patch# = group*64 (INT 0, CRD 64, PRA 128, PRB 192), verified from the unit's bytes.
Synth list = card stacks per device (`device`/`entry` in synth_t; encoder browses a stack, button opens). The selected card slides to its slot (done). Paging was tightened in every synth except the D-110 (user: leave it). Next: Scene / Volca Drum ideas, or whatever the user brings.
U-110 (2026-10-20, committed; editing, read, IMPORT 001-064 and REC > TO U-110 tested on the unit): one entry, patch = 6 parts (Shift + Group A-F / Step arrows); A PART (+ PROGRAM CHANGE page), B ENV / PITCH, C LFO, D COMMON; pads play the part's Rx channel; CONTROL CH setting = device ID, model ID 0x23 (from Edisyn only), unit needs SETUP: MIDI EXCLUSIVE ON (and for IMPORT PGM CHANGE ON, for REC > TO U-110 PATCH > WRT > MEM. P = OFF, else it silently ignores the write). After an output mode DT1 the unit needs ~400 ms and resets the parts' output assigns (shown as `*` = OFF on its MIDI monitor page), so those are re-sent. Reference: `reference/edisyn/` (clone of eclab/edisyn, Apache 2.0: Wavestation, U-110, JV-880, D-110 layouts; no NS5R). The U-110 manual is a scanned PDF (read pages as images). Read = RQ1 01 01 00 (temp patch; stored ones can't be asked for, IMPORT sends a Program Change first); store = two DT1s to 02 (2n) 00 / 02 (2n+1) 00; bulk dumps are nibble-packed, layout from Edisyn's RolandU110.java (verified both ways on the unit). The Store screen's default name is now the loaded factory patch's / unit sound's name instead of "User N" (all synths). Wavestation SR PATCH (2026-10-20, committed; display/pictures seen on the unit, sound path not yet confirmed): notes in `docs/WAVESTATION.md`. Groups = functions (A WAVE, B PITCH, C AMP, D ENV 1, E FILTER, F LFO, G MIX, H PATCH), the wave (A-D) by Shift + Group A-D / Step arrows (inactive waves skipped), STRUCTURE on H. SR needs MIDI page Param = RECEIVE and the same Basic Channel; EDIT BANK / EDIT PATCH = the patch part 1 plays, the editor's values go over it (edit buffer only; storing to RAM needs bank dumps, not done: midi.c queue 256 events / input buffer 1024 bytes must grow, paced chunks). Envelopes have the Volca-style dotted trail (`jv880_draw_env_ex`); the vector picture follows the structure (diamond for 4 waves, AC axis with a row per segment for 2, none for 1); RST STEP / RST VECT press twice. Live vector = CC 16 / 17 (the SR's MIDI REMAP defaults). Next: wave sequences (user: critical; SR has no single-sequence write, Edisyn builds them with erase + insert-step commands), PERF, bank dump read/store. Waiting: NS5R (`reference/N5SR/`), Volca Drum, a MIDI joystick of the user's own (CC 16/17 through a splitter into MIDI IN).
Boards 19 (House) and 97
(Experience III) have no waveform list in `reference/JV880/` - left as they are on purpose (names show as numbers).
Ideas waiting: synth list as per-device card stacks; Scene = saved multi-synth scenes; Volca Drum.
