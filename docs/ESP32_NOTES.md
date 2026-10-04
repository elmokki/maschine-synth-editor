# ESP32-S3 firmware notes

PlatformIO + ESP-IDF (v5.5.5 via pioarduino's `framework-espidf`). Board: generic
ESP32-S3-WROOM-1 **N16R8** (16MB flash, 8MB octal PSRAM, 44-pin, USB Type-C) — "same board as
the EZ1 project, different batch" per the user. Matches PlatformIO's `esp32-s3-devkitc1-n16r8`
board definition.

## Step 1: device detection only

`src/main.c` installs ESP-IDF's native USB Host Library (`usb/usb_host.h`), waits for a device
to enumerate, opens it, reads the device descriptor, and prints VID/PID — flagging it if it
matches the Maschine Mk2 (`0x17CC`/`0x1140`, see `docs/PROTOCOL.md`). Deliberately does *not*
touch the HID interface yet — this step is only to confirm the board's USB OTG + VBUS power
setup (the jumper fix mentioned when starting this) actually lets a downstream device enumerate
at all, before building any protocol logic on top of it.

Builds cleanly (`pio run`, verified in this environment: RAM 4.4%, Flash 2.8%).

**Confirmed working on real hardware (2026-10-04):** board powered from its COM/UART USB-C port
(`/dev/ttyACM0` on the PC), Maschine plugged into the OTG port via a USB-C→A OTG adapter. The
Maschine powers up from the board's VBUS (OTG jumper fix works) and enumerates ~400 ms after
`app_main()`: `New device at addr 1: VID=0x17CC PID=0x1140`. PSRAM (8MB octal) and 16MB flash
are also detected correctly at boot. Never plug the OTG port into a PC while the jumper is
bridged — the board would back-feed 5V into the PC's port.

## Step 2: HID round-trip — working

`src/main.c` claims IF2 and confirms both directions on hardware: pad presses (`0x20`) light the
pressed pad white via `0x80` (correct pad every time), and button / main encoder / endless encoder
events from `0x01` are logged by name. Two full-speed-specific findings, neither visible on the
PC (which talks to the Maschine at high speed through the kernel's HID driver):

- **`0x01` never arrives on the interrupt IN pipe.** The ESP32-S3 is a full-speed-only host, so
  the interrupt endpoint (`bInterval` 1) is polled once per 1 ms frame. The Maschine streams pad
  reports continuously at 500 Hz, and each `0x20` report is 65 bytes = 2 packets (64 + 1), so
  the pad stream fills every single poll (measured: exactly 500 pad reports/s, zero `0x01`
  reports even while pressing buttons). Fix: read `0x01` with a HID **GET_REPORT** control
  request on EP0 instead (`bmRequestType 0xA1, bRequest 0x01, wValue 0x0101, wIndex 2,
  wLength 25`). The Maschine answers it fine, ~170 Hz with no failures, and it doesn't slow the
  pad stream.
- **ESP-IDF's HCD asserts on multi-packet interrupt IN transfers ending in a short packet**
  (`assert failed: _buffer_parse_intr hcd_dwc.c:2437`, crash loop on connect). Every `0x20`
  report ends that way. Fix: submit IN transfers of exactly one MPS (64 bytes) and reassemble
  reports in software — a report is complete on a short packet or once it reaches its known
  length (`0x01` 25, `0x20` 65).

## Step 3: displays — working

`src/display.c` keeps a 1bpp framebuffer per display, tracks which 8-row chunks changed, and
`main.c` streams only those as `0xE0`/`0xE1` reports (265 bytes, 5 packets each) over the
interrupt OUT pipe, sharing it with pad LEDs (pad LEDs get priority). ESP-IDF's HCD handles
these multi-packet interrupt OUT transfers fine — the assert above is IN-only.

Fonts are DejaVu Sans Condensed pre-rendered to bitmaps by `tools/gen_font.py` (generates
`src/fonts.c`/`fonts.h`; 11pt → 14 px line, 20pt → 24 px line). Test UI: 8 parameters, one per
endless encoder, in a 64 px column right under it (name, value 0-127, bar), 10 encoder units per
step (~130 steps per turn). Confirmed on hardware: layout lines up with the encoders, updates
feel responsive, and pads keep working while turning encoders.

## Step 4: DIN MIDI — working both ways

`src/midi.c` claims the Maschine's standard USB-MIDI interface (IF1, bulk) alongside the HID
interface, hand-rolled (4-byte USB-MIDI event packets, cable 0) rather than via TinyUSB — it's a
few dozen lines with ESP-IDF's host library. Pads send Note On/Off (pad 0 = note 36, channel 1,
crude velocity from pressure at detection), and the 8 test parameters send CCs; DIN IN is logged.

**Full-speed gotcha:** the Maschine reports its bulk endpoints with `wMaxPacketSize` **512**
even when enumerated at full speed (it serves its high-speed descriptors at both speeds; bulk
at full speed can only be 64). ESP-IDF refuses the allocation (`EP MPS (512) exceeds supported
limit (408)`), so the interface claim fails. Fix: patch the cached endpoint descriptors down to
64 before claiming (`fix_fs_bulk_mps` in `midi.c`).

Confirmed on hardware via a PC USB-MIDI interface (ALSA `32:0` / `hw:4,0,0`) cabled to the
Maschine's DIN jacks: messages sent with `amidi` arrived at the ESP32 intact (note, CC, program
change), and all 16 pads (notes 36-51, every Note On paired with its Note Off) and encoder CCs
showed up in `aseqdump`. Note: the Maschine buffers DIN IN data while nobody reads IF1, and
delivers it once the host starts reading.

## Step 5 (current): MiniMax editor + patch library — working

**Patch library on LittleFS.** `partitions.csv`: 4MB app + ~12MB `storage` (LittleFS, via the
`joltwallet/littlefs` component), mounted at `/lfs` by `src/storage.c`. `pio run -t uploadfs`
writes `data/` into it. Each synth's patches live under their own directory; a patch file is
the raw MIDI that restores the sound (SysEx dump for SysEx synths, a CC/poly-AT stream for the
Plugiator), so loading is the same for every synth.

**MiniMax data comes from the use-audio Plug-in Manager's own files** (found in the Wine prefix,
copied to the gitignored `reference/`): `config.xml` is the full parameter map and
`presets/default.xml` stores each preset as raw CC/poly-AT values. `tools/plugiator_import.py`
turns them into `src/minimax_params.c/.h` (committed — MIDI implementation facts only) and
`data/plugiator/minimax/NNN Name` (gitignored — use-audio's factory presets). 6-position knobs
use 0/22/44/66/88/110, switches 0/127.

**Architecture:** `src/editor.c` is the generic editor (UI, navigation, patch library, settings)
and knows nothing about any particular synth; each synth is a driver behind `src/synth.h`
(`src/synth_minimax.c` is the first): it owns the parameter values and provides the pages and
settings, formatting/stepping/sending of parameters, an optional picture per parameter
(waveforms; envelopes later), and patch load/save. A second synth = a new driver file.

**Synths** (Scene button): one column per driver, its display button switches (asks SURE? if
the patch has unsaved edits). Switching (and every boot) runs the driver's `activate` — for
Plugiator plug-ins a Program Change (Setup: PLUGIN PC; on this box MiniMax 101, B4000 102, Lightwave 103 - the manual has
B4000/Lightwave the other way round, it depends on the install order) plus a confirming note 100 ms later,
which loads the plug-in in ~1 s — waits 1.5 s, then sends
that synth's remembered patch. The active synth is kept in `/lfs/state/active_synth`.
Plugiator drivers share `src/plugiator.c` (CC/poly-AT parameters, value scales, stepped
controls, pictures, patch files); `synth_minimax.c` / `synth_b4000.c` only hold their pages,
settings and virtual parameters. Navigate moved to Sampling.

**B4000** (`synth_b4000.c`): Upper/Lower manuals as parallel 2-page sections (8 drawbars, then
1' + volume/octave/split/vibrato), Pedal, Vibrato/Perc, Rotor (horn | bass), Organ, Effects.
Drawbars are drawn as drawbars (9 positions, MIDI 0/15/31/.../127, brown/white/black handles).
From the GUI's MIDI monitor: CC73 = vibrato selector (Hammond order V-1 C-1 V-2 C-2 V-3 C-3 =
120 10 30 50 70 90), CC74/CC71 = lower/upper vibrato on/off — the factory preset file has
CC73/CC74 swapped (the Plug-in Manager sends them as stored), the importer swaps them back.
Swell (CC11) is reversed: 0 = loud. Octaves 0/1/2 = -1/0/+1, split keys = note numbers.

**Setup** (Control button): the active synth's settings in the same column layout, one per
encoder — every driver starts with `CHANNEL` and `PAD LOW` (lowest pad note), Rolands will add a
device ID. Saved as `NAME=value` lines in `/lfs/settings/<synth>` 5 s after the last change. An
input channel (for DIN IN) will be added when a synth needs it; the Plugiator doesn't.

**Editor** (`src/editor.c`), the navigation model meant for every synth:
- Group buttons A-H select a section, pressing the current one again steps to its next page;
  the main encoder and the top arrow pair step through all pages; Navigate (held) shows the
  section list. Parallel sections keep the page position (OP1 EG → OP3 EG).
- Group LEDs: dim blue = section exists, dim violet = it has several pages; the selected one is
  cyan, and its page shows on the button's two LEDs (page 1 = left, 2 = right, 3 = whole button).
- MiniMax sections follow the plug-in GUI: the Main panel first in signal-chain order
  (A Oscillators, B Mixer, C Filter, D Loudness), then E Controllers, then the Add panel
  (F Additional, G Effects: Chorus / Delay / Delay Sync). Each switch sits above the knob it
  belongs with (the Decay switch above A DECAY, not in Controllers as on the panel).
- 8 encoders = the page's 8 parameters (6-position knobs step every ~1/16 turn, waveforms drawn
  as a small picture), display buttons = the page's switches.
- Header row: section/page and patch on the left display, prev/next page on the right. The
  arrow pair next to Enter browses patches (or the main encoder, while Browse is toggled on);
  a browsed patch shows inverted in the header (plain again when back on the loaded one), Enter
  loads it, any parameter encoder cancels browsing.
- Rec = store screen: save as new (`User NNN`, numbers from 100) or over a user patch, RENAME
  (pads become a phone keypad, `src/textentry.c`), DELETE (press twice). Factory patches
  (000-099) are read-only.
- The last loaded patch is remembered (`/lfs/state/minimax_last`, written 5 s after a patch
  load and only if it changed) and sent again on the first connect after boot; the synth's
  default patch (`099 Init`) if nothing is remembered or it's gone. Deleting the loaded patch
  loads the one before it in number order.
- Erase = init: first press turns the left header into an inverted "PRESS ERASE AGAIN: INIT
  PATCH" prompt (Erase lit), a second press within 3 s loads the synth's default patch, any
  other control cancels. Erase held + turning an encoder = that parameter back to its init
  value. Rec + Erase together (the panel's "Replace") = revert the loaded patch to its stored
  state; Rec therefore opens the store screen on release. Editing the loaded patch adds " *"
  to its name in the header. Mute = panic (All Sound Off + All Notes Off), on every screen.
- Pads: the note goes out 3 pad reports (~6 ms) after the press threshold, with velocity from
  the peak pressure seen meanwhile.
- The Plugiator can't report its state (no MIDI OUT on DIN), so values show "--" until a patch
  is loaded or the parameter is touched. Patch load sends every parameter (verified byte-exact
  on the PC side).
- Every button that does something is lit dimly, brightly when on/active. The mono button LED
  slots were mapped with a temporary on-device wizard, see `docs/PROTOCOL.md`. Boxes standing
  for physical buttons/pads have 2 px rounded corners, like the buttons.
- Fonts: Spleen pixel fonts (5x8, 6x12, 8x16, 12x24, BSD license) converted from BDF by
  `tools/gen_font.py`; they're far crisper on the 1-bit panel than anti-aliasing-designed TTFs.

**⚠️ `pio run -t uploadfs` rewrites the whole LittleFS partition** — it wipes user patches and
the remembered patch. Only use it on a fresh board. To add a synth's factory patches to a board
in use, `tools/lfs_merge.py <data subdir>` reads the partition back (a backup), adds the files
in a PC-side copy and writes that back only with `--write`.

**Flash writes vs. USB:** the very first patch save made every USB pipe fail with transfer
errors (likely a long flash erase holding off the USB host interrupt). All pipes now halt,
flush and clear the endpoint and resubmit on error (`src/usb_recover.h`); the error hasn't
recurred since, so that recovery path is still untested on real hardware.

Not yet tested with the Plugiator itself connected (verified via the PC MIDI interface only).

## Flash-size mismatch warning — fixed, and why it happened

`pio run -t compiledb` initially warned `Flash memory size mismatch detected. Expected 8MB,
found 2MB!`, then still `Expected 16MB, found 2MB!` even after switching `board` to the correct
N16R8 definition. **The board JSON's flash-size metadata is not what feeds the Kconfig flash-size
default** — ESP-IDF's generated `sdkconfig` falls back to a 2MB default regardless of which
`board` is selected unless `sdkconfig.defaults` explicitly sets it. Fixed by adding
`sdkconfig.defaults` with `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` (and `CONFIG_SPIRAM_MODE_OCT=y`
for the N16R8's octal PSRAM, which also isn't the ESP32-S3 default — that's Quad). Confirmed
clean after: no warning, `pio run` shows the full 16MB (16777216 bytes) as available flash.

## Next steps (after device detection is confirmed working)

1. ~~Claim the HID interface and do a round-trip~~ — done, see Step 2.
2. ~~DIN MIDI via the USB-MIDI class interface~~ — done, see Step 4.
3. Port the rest of `docs/PROTOCOL.md` (pads, buttons, encoders, displays) incrementally, cross-
   checking against the already-verified behavior in `pc_reference/` rather than re-deriving
   anything from scratch.
