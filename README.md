# Maschine Mk2 → ESP32-S3 synth editor controller

Goal: drive a full-size Native Instruments Maschine Mk2 (the hardware controller) from an
ESP32-S3 over USB host, to use as a **programmer/editor controller for synths** — primarily
Volca FM2, Roland D-110, Roland JV-880, and Plugiator. Not primarily a pad controller (that
part mostly came along for free while reverse-engineering the protocol, and works, but isn't
the point).

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
(bitmap fonts, test synth-editor UI) and DIN MIDI in/out all work on real hardware. A few
full-speed-only quirks had to be worked around — see
**[`docs/ESP32_NOTES.md`](docs/ESP32_NOTES.md)**. Next: actual synth-editor logic.

## Layout

- `docs/PROTOCOL.md` — the protocol reference. Read this first.
- `docs/ESP32_NOTES.md` — firmware notes, step by step, incl. the full-speed workarounds.
- `src/` — ESP32-S3 firmware (`main.c` HID side, `display.c`, `midi.c`, generated `fonts.c`).
- `tools/gen_font.py` — regenerates `src/fonts.c`/`fonts.h` from a TTF.
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
