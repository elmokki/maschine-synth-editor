# ESP32-S3 firmware notes

PlatformIO + ESP-IDF (v5.5.5 via pioarduino's `framework-espidf`). `platformio.ini` targets
board `esp32-s3-devkitc-1` as a **guess** — change it if your actual board differs.

## Step 1 (current): device detection only

`src/main.c` installs ESP-IDF's native USB Host Library (`usb/usb_host.h`), waits for a device
to enumerate, opens it, reads the device descriptor, and prints VID/PID — flagging it if it
matches the Maschine Mk2 (`0x17CC`/`0x1140`, see `docs/PROTOCOL.md`). Deliberately does *not*
touch the HID interface yet — this step is only to confirm the board's USB OTG + VBUS power
setup (the jumper fix mentioned when starting this) actually lets a downstream device enumerate
at all, before building any protocol logic on top of it.

Builds cleanly (`pio run`, verified in this environment: RAM 4.4%, Flash 2.8%). Not yet flashed
to real hardware or tested against the real device at time of writing.

## TODO before flashing to real hardware

- **Flash size mismatch**: `pio run -t compiledb` warned `Flash memory size mismatch detected.
  Expected 8MB, found 2MB!` — this is a static mismatch between the `esp32-s3-devkitc-1` board
  definition's assumed 8MB flash and ESP-IDF's generated default `sdkconfig`, not a live reading
  from actual connected hardware (none was connected when this was seen). **Check your actual
  board's real flash size** (`esptool.py flash_id`, or the board's own specs) and set it
  correctly — either pick a more accurate `board` in `platformio.ini`, or override explicitly
  (e.g. `board_upload.flash_size` / via `pio run -t menuconfig` → Serial Flasher Config). Getting
  this wrong can affect partition table placement and boot — don't skip it.
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
