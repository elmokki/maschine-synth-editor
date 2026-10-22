# Korg Wavestation SR - notes for the editor (2026-10-20, planning)

Sources: `reference/wavestationSR/WS_SRReferenceGuide_E.pdf` (scanned: read pages as images; PDF page = printed
page + 3; Appendix 4 MIDI SysEx format from p.~172, parameter numbers p.183-185, MIDI REMAP p.100, MIXENV
p.103-107), Edisyn `reference/edisyn/edisyn/synth/korgwavestation/` (`SYSEX.txt`, `KorgWavestationAbstract.java`,
`...Patch.java`, `...Performance.java`, `...Sequence.java`, `...Joystick.java`). Edisyn's SR editor is marked
librarian-tested.

## Data
- Banks (12): RAM1-3, ROM4-11, Card. Patch numbers 0-34 (35 per bank), performances 0-49. The bank numbers differ
  between parameter messages (`edisynToWSBank`) and bank dumps (`edisynToWSExpBank`) - tables in the Abstract class.
- Patch = 426 bytes (852 as nibbles; dump message 861 bytes: `F0 42 3n 28 40 bank num <852> sum F7`). Performance =
  181 bytes (371-byte message). Names 15 characters. Whole bank dump (`4C`, 35 x 852 + 8 = 29.8 KB).
- Messages: dump request `F0 42 3n 28 10 bank num F7` (patch); bank request `... 1C bank F7`; parameter change
  `F0 42 3n 28 41|42|43 lo hi <value as ASCII text, "+5"/"-3", space = 7F> 00 F7` (41: params 0-379, 42: 380-406,
  43: 407+ "SR expanded"); `n` = channel.
- Parameter edits go to the part's patch in the edit buffer: Performance mode, patch selected into the part with
  PART_PATCH_BANK / PART_PATCH_NUM (wait ~300 ms), CURRENT_WAVE (param 431 WAVE_WAVE, 0-3) selects the oscillator
  before an oscillator parameter, MIX_ENV_POINT (0-4) before a mix-envelope X/Y/RATE. Edisyn waits 40 ms per
  parameter ("else packets drop").
- Storing a patch into the unit by single-patch write does NOT work on the SR (Edisyn: "we'll attempt a write but
  fail"). The way: read the whole bank (`1C`), replace one patch, send the bank back (`4C`), RAM banks only.
- Wave sequences: no single-sequence SysEx. Edisyn builds them by SR commands (erase, insert step after step) -
  `SYSEX.txt` describes its own file format. Leave out of the first version.

## Joystick / vector
- The SR has no joystick: MIDI controllers drive it. MIDI REMAP page: "Map to JoyAC" default **CC 16** (x axis,
  A-C), "Map to JoyBD" default **CC 17** (y axis, B-D), FX switch CC 12; both settable (params REMAP_TO_JOY_X/Y 380/381
  - ignored by the keyboard Wavestation). Mix envelope AC/BD = -127..+127 (diamond-shaped area, clipped at the edge).
- So: the two encoders (and the user's own MIDI joystick through a splitter into MIDI IN) send CC 16/17 on the
  SR's channel. No SysEx needed to play the vector. The editor can show the position by sniffing CC 16/17 on MIDI IN.

## Firmware limits (lifted)
- Output queue 512 events (a patch dump needs ~290); a bank (29828 bytes) goes out in paced parts (`midi_send_sysex_part`).
- Input SysEx buffer 32 KB in PSRAM; the main task stack 8192 (the import's file work overflowed 3584).

## How Edisyn does it (read 2026-10-20; its KorgWavestationPatch.html says the same)
- Patches are auditioned in a performance: Edisyn's "Set up Test Performance in RAM 1 Slot 0" sends a single-part performance
  dump (`F0 42 3n 28 49 bank num <362 nibbles> sum F7`, 8 parts x 18 bytes after a 16-byte name and 21 FX bytes; parts off
  = patch number 0xFF) to the edit buffer, then the patch dump (`40`, 861 bytes) goes to the part's patch.
- A bank holds 50 performances, 35 patches and 32 wave sequences (+ 500 steps of memory); the dump types are separate.
  PCM waves (486) exist only in ROM and on cards; a patch's wave number 0-31 is a wave sequence of the wave's bank.
- EXECUTE_WRITE (466) "does nothing" (Edisyn's comment); writing a patch into RAM = bank dump (Edisyn says press Yes on the SR; not needed in practice).
- MIDI page: Param (SysEx parameters) must be RECEIVE (default DISABLE) - dumps and dump requests work regardless.
- Wave sequence steps: duration and crossfade in units of about 24 ms; loop (FWD or B/F), start step + modulation.

## Wave sequences (SEQ entry, 2026-10-21)
- Bank dump: request `F0 42 3n 28 0C bank F7`, reply `F0 42 3n 28 54 bank <8784 bytes as nibbles> sum F7` (17576 bytes).
  The block is 32 sequences x 16 bytes, 501 steps x 16, 32 names x 8 (big-endian words; Edisyn's `Block`). A sequence's
  steps are a linked list (sequence link = first step, step fLink = next, 0 = end); loop start / end are 0-based in the
  dump (Edisyn shows +1); a step's wave = low byte + 256 if the high byte is 0x81, coarse >= 48 = expanded PCM (coarse - 72,
  wave + 365), the wave index = that - 32; loop count bit 7 = back and forth. What the sequence's "start step" byte means
  is unverified (Edisyn never reads it). The dump has no wave bank per step (the editor's BANK knob, ROM / CARD, is only
  a mark for now).
- Library: `/lfs/wsseq/NNN name`, NNN = bank * 32 + n + 1 (file layout in `src/ws_seq.h`); the engine is `ws_banks.c`
  (`ws_kind_seq`: import only, `extract` lifts one sequence out of the dump).
- Listening: a sequence is only heard through a patch (wave number 0-31 of its bank). The editor sends Edisyn's test
  performance and an Init patch with wave A = the sequence into the edit buffer (RAM 1 #0 is only the name), about 1.6 s
  of messages; the SR's own version plays, our edits do not. The loading screen stays 3 s and blocks input (a second load
  in the middle of the messages froze the editor once).
- Writing (done 2026-10-21, tested): the SR accepts the whole sequence bank as a dump (reply type `54` sent back, status 23
  + SUCCESS as for patches; Edisyn's own docs say so, it just never did it). `ws_seq_put` replaces one sequence in the dump;
  the bank as it was goes to `/lfs/ws/backup/seq02` first. Sequences have no edit buffer, a write is permanent. First the editor wrote only RAM 3 #31; since 2026-10-22 any
  RAM 1-3 slot can be chosen (96 slots on the Store screen, starting at RAM 3 #31; the card is left out), one backup file a bank. What the unit's own sequences show (read
  from RAM 1 and 2): links fLink / bLink with 0 for none, steps 0 and 500 sentinels (f = b = 65535), 499 usable steps for
  all 32 sequences; a step's lLink = 65534 at the loop's first step, the loop's first step's number at the loop's last
  step (the last step wins if one step is both), 65535 elsewhere; sLink = the start step's number, the start byte is
  0-based (like loop start / end); time = the durations' sum + the last step's crossfade / 2 (matched every sample);
  start time = ? (0 when start is the first step; the editor writes the durations before the start step, a guess);
  a step's mod index 127 and dyno mod 0 without modulation (with a modulation amount the indexes are i * dyno-ish and
  dyno mod depends on something not understood: the amount is NOT written, left 0); mod source byte = 0x80 | source.
  A step's wave: value = low byte + 256 if high byte 0x81; the SR codes waves >= 365 as expanded (low = wave - 365,
  coarse + 72). RAM 1 of the author's unit had garbage in sequences 11-13 (nonsense links): chains are walked with a
  guard. The dump has no wave bank per step, so card waves (the BANK knob = CARD) are refused when writing.
  Edisyn's command route (erase, insert steps, parameters: minutes, immediate, buggy) is not used.
