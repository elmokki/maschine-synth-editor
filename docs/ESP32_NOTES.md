# ESP32-S3 firmware notes

PlatformIO + ESP-IDF (v5.5.5 via pioarduino's `framework-espidf`). Board: generic
ESP32-S3-WROOM-1 **N16R8** (16MB flash, 8MB octal PSRAM, 44-pin, USB Type-C) — "same board as
the EZ1 project, different batch" per the user. Matches PlatformIO's `esp32-s3-devkitc1-n16r8`
board definition.

## Step 1 (current): device detection only

`src/main.c` installs ESP-IDF's native USB Host Library (`usb/usb_host.h`), waits for a device
to enumerate, opens it, reads the device descriptor, and prints VID/PID — flagging it if it
matches the Maschine Mk2 (`0x17CC`/`0x1140`, see `docs/PROTOCOL.md`). Deliberately does *not*
touch the HID interface yet — this step is only to confirm the board's USB OTG + VBUS power
setup (the jumper fix mentioned when starting this) actually lets a downstream device enumerate
at all, before building any protocol logic on top of it.

Builds cleanly (`pio run`, verified in this environment: RAM 4.4%, Flash 2.8%). Not yet flashed
to real hardware or tested against the real device at time of writing.

## Flash-size mismatch warning — fixed, and why it happened

`pio run -t compiledb` initially warned `Flash memory size mismatch detected. Expected 8MB,
found 2MB!`, then still `Expected 16MB, found 2MB!` even after switching `board` to the correct
N16R8 definition. **The board JSON's flash-size metadata is not what feeds the Kconfig flash-size
default** — ESP-IDF's generated `sdkconfig` falls back to a 2MB default regardless of which
`board` is selected unless `sdkconfig.defaults` explicitly sets it. Fixed by adding
`sdkconfig.defaults` with `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` (and `CONFIG_SPIRAM_MODE_OCT=y`
for the N16R8's octal PSRAM, which also isn't the ESP32-S3 default — that's Quad). Confirmed
clean after: no warning, `pio run` shows the full 16MB (16777216 bytes) as available flash.

## Still TODO before flashing to real hardware

- Confirm VBUS is actually present on the USB port after the OTG jumper fix, ideally with a
  meter, before relying on the firmware's enumeration result to tell you that indirectly.

## Next steps (after device detection is confirmed working)

1. Claim the HID interface (interface 2 in `docs/PROTOCOL.md`'s topology table) and do a single
   round-trip: read one `0x01` report, light one pad via `0x80`.
2. Separately, bring up the USB-MIDI class interface (interface 1) for DIN MIDI — this is
   architecturally independent of the HID work (see `docs/PROTOCOL.md`), consider using TinyUSB's
   host MIDI class driver for it rather than hand-rolling anything, since it's a completely
   standard USB-MIDI class interface.
3. Port the rest of `docs/PROTOCOL.md` (pads, buttons, encoders, displays) incrementally, cross-
   checking against the already-verified behavior in `pc_reference/` rather than re-deriving
   anything from scratch.
