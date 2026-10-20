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

## Firmware limits to lift
- `midi.c` output queue is 256 events (~255 bytes of SysEx per message): a patch dump (861 bytes) needs ~290
  packets. Either a bigger queue (512) or streaming a message out in chunks from `tick()` (needed for a bank dump).
- `midi.c` input SysEx buffer `THRU_SYSEX_MAX` is 1024: a bank dump reply is 29.8 KB (static buffer or PSRAM).

## How Edisyn does it (read 2026-10-20; its KorgWavestationPatch.html says the same)
- Patches are auditioned in a performance: Edisyn's "Set up Test Performance in RAM 1 Slot 0" sends a single-part performance
  dump (`F0 42 3n 28 49 bank num <362 nibbles> sum F7`, 8 parts x 18 bytes after a 16-byte name and 21 FX bytes; parts off
  = patch number 0xFF) to the edit buffer, then the patch dump (`40`, 861 bytes) goes to the part's patch.
- A bank holds 50 performances, 35 patches and 32 wave sequences (+ 500 steps of memory); the dump types are separate.
  PCM waves (486) exist only in ROM and on cards; a patch's wave number 0-31 is a wave sequence of the wave's bank.
- EXECUTE_WRITE (466) "does nothing" (Edisyn's comment); writing a patch into RAM = bank dump, then press Yes on the SR.
- MIDI page: Param (SysEx parameters) must be RECEIVE (default DISABLE) - dumps and dump requests work regardless.
- Wave sequence steps: duration and crossfade in units of about 24 ms; loop (FWD or B/F), start step + modulation.
