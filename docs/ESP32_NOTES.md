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

## Step 3 (current): displays — working

`src/display.c` keeps a 1bpp framebuffer per display, tracks which 8-row chunks changed, and
`main.c` streams only those as `0xE0`/`0xE1` reports (265 bytes, 5 packets each) over the
interrupt OUT pipe, sharing it with pad LEDs (pad LEDs get priority). ESP-IDF's HCD handles
these multi-packet interrupt OUT transfers fine — the assert above is IN-only.

Fonts are DejaVu Sans Condensed pre-rendered to bitmaps by `tools/gen_font.py` (generates
`src/fonts.c`/`fonts.h`; 11pt → 14 px line, 20pt → 24 px line). Test UI: 8 parameters, one per
endless encoder, in a 64 px column right under it (name, value 0-127, bar), 10 encoder units per
step (~130 steps per turn). Confirmed on hardware: layout lines up with the encoders, updates
feel responsive, and pads keep working while turning encoders.

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
2. Separately, bring up the USB-MIDI class interface (interface 1) for DIN MIDI — this is
   architecturally independent of the HID work (see `docs/PROTOCOL.md`), consider using TinyUSB's
   host MIDI class driver for it rather than hand-rolling anything, since it's a completely
   standard USB-MIDI class interface.
3. Port the rest of `docs/PROTOCOL.md` (pads, buttons, encoders, displays) incrementally, cross-
   checking against the already-verified behavior in `pc_reference/` rather than re-deriving
   anything from scratch.
