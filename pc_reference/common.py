"""
Shared helpers for talking to the Maschine Mk2 over raw hidraw, on Linux.

See ../docs/PROTOCOL.md for the full writeup of what every report means and
how it was verified. This module only implements the mechanics (chunk
framing, bit packing) that every script needs.

Find your device node with e.g.:
    for f in /dev/hidraw*; do
        udevadm info -q property -n "$f" | grep -q '17CC.*1140' && echo "$f"
    done
"""

import glob
import os
import subprocess


def find_device_path(vid="17CC", pid="1140"):
    """Locate the hidraw node for the Maschine Mk2 by VID/PID, instead of
    hardcoding a path that will differ on other machines/boots."""
    for path in sorted(glob.glob("/dev/hidraw*")):
        name = os.path.basename(path)
        try:
            out = subprocess.run(
                ["udevadm", "info", "-q", "property", "-n", path],
                capture_output=True, text=True, check=True,
            ).stdout
        except subprocess.CalledProcessError:
            continue
        if f"{vid}p{pid.upper()}".lower() in out.lower() or (
            vid.lower() in out.lower() and pid.lower() in out.lower()
            and "HID_ID" in out
        ):
            return path
    raise RuntimeError(
        "Could not find the Maschine Mk2 hidraw device. "
        "Check it's plugged in, or pass the path explicitly."
    )


def open_device(path=None, read_write=True):
    if path is None:
        path = find_device_path()
    flags = os.O_RDWR if read_write else os.O_RDONLY
    return os.open(path, flags)


# ---------------------------------------------------------------------------
# Display (0xE0 / 0xE1): 256x64px, 1bpp, 32 bytes/row, 8 chunks of 256 bytes.
# See PROTOCOL.md for how this was derived/confirmed. Known hw limitation:
# column x=255 is physically not visible - keep real content in x=0..254.
# ---------------------------------------------------------------------------

DISPLAY_WIDTH = 256
DISPLAY_HEIGHT = 64
DISPLAY_STRIDE = DISPLAY_WIDTH // 8  # 32
DISPLAY_USABLE_WIDTH = 255  # x=255 is cropped by the panel/bezel on this unit


def new_framebuffer():
    return bytearray(DISPLAY_STRIDE * DISPLAY_HEIGHT)


def set_pixel(fb, x, y, on=True):
    if x < 0 or x >= DISPLAY_WIDTH or y < 0 or y >= DISPLAY_HEIGHT:
        return
    idx = DISPLAY_STRIDE * y + (x >> 3)
    bit = 0x80 >> (x & 7)
    if on:
        fb[idx] |= bit
    else:
        fb[idx] &= (~bit) & 0xFF


def send_framebuffer(fd, fb, display=0xE0, prev_fb=None):
    """Send a 2048-byte framebuffer as 8 chunked HID output reports.
    If prev_fb is given, only chunks that actually changed are sent
    (cheap dirty-chunk optimization for animated updates)."""
    for chunk in range(8):
        start = chunk * 256
        chunk_data = fb[start:start + 256]
        if prev_fb is not None and prev_fb[start:start + 256] == chunk_data:
            continue
        header = bytes([display, 0x00, 0x00, chunk * 8, 0x00, 0x20, 0x00, 0x08, 0x00])
        os.write(fd, header + bytes(chunk_data))


def clear_display(fd, display=0xE0):
    blank = new_framebuffer()
    send_framebuffer(fd, blank, display)


def image_to_framebuffer(img):
    """Convert a PIL Image (any mode) to our display's bit format."""
    img = img.convert("1")
    fb = new_framebuffer()
    for y in range(DISPLAY_HEIGHT):
        for x in range(DISPLAY_WIDTH):
            if img.getpixel((x, y)):
                set_pixel(fb, x, y, True)
    return fb


# ---------------------------------------------------------------------------
# Report 0x01 IN: buttons (bytes 0-6) + main encoder (byte 7) +
# 8 touch-encoders (bytes 8-23). All offsets below are BODY offsets, i.e.
# already assuming byte 0 of the raw read (the report-ID byte) was stripped.
#
# ** Gotcha hit once already: raw hidraw reads include the report-ID byte
# ** at index 0. If you index the raw buffer directly instead of the body,
# ** everything is off by one and encoder values look like garbage (seen:
# ** values up to 65535 instead of 0-999). Always slice off byte 0 first.
# ---------------------------------------------------------------------------

BUTTON_NAMES = [
    # byte 0
    "DisplayBtn1", "DisplayBtn2", "DisplayBtn3", "DisplayBtn4",
    "DisplayBtn5", "DisplayBtn6", "DisplayBtn7", "DisplayBtn8",
    # byte 1
    "Control", "Step", "Browse", "Sampling", "ArrowLeft", "ArrowRight", "All", "Auto",
    # byte 2 (bit 23 unused)
    "Volume", "Swing", "Tempo", "ArrowLeft2", "ArrowRight2", "Enter", "NoteRepeat", None,
    # byte 3
    "GroupA", "GroupB", "GroupC", "GroupD", "GroupE", "GroupF", "GroupG", "GroupH",
    # byte 4
    "Restart", "StepLeft", "StepRight", "Grid", "Play", "Rec", "Erase", "Shift",
    # byte 5
    "Scene", "Pattern", "PadMode", "Navigate", "Duplicate", "Select", "Solo", "Mute",
    # byte 6 - unmapped (bit 0 observed always-high, likely not a real button)
    None, None, None, None, None, None, None, None,
]


def parse_report01(body):
    """body = the 24-byte report 0x01 payload, WITHOUT the report-ID byte.
    Returns (button_states: list[bool] len 56, main_encoder: int 0-15,
    touch_encoders: list[int] len 8, each 0-999ish)."""
    buttons = []
    for byte_i in range(7):
        b = body[byte_i]
        for bit_i in range(8):
            buttons.append(bool((b >> bit_i) & 1))

    main_encoder = body[7] & 0x0F  # low nibble; high nibble untested

    touch_encoders = []
    for i in range(8):
        lo = body[8 + i * 2]
        hi = body[9 + i * 2]
        touch_encoders.append(lo | (hi << 8))

    return buttons, main_encoder, touch_encoders


# ---------------------------------------------------------------------------
# Report 0x20 IN: pads. 32 x 16-bit slots; pad identity is IN the data
# (high nibble of the odd byte), not implied by slot position. Each pad
# reports in two of the 32 slots per read (harmless duplication).
# ---------------------------------------------------------------------------

def parse_report20(body):
    """body = the report 0x20 payload (64 bytes), WITHOUT the report-ID byte.
    Returns a dict {pad_idx(0-15): pressure(0.0-1.0)} - last slot wins if a
    pad appears twice with different nearby values (they're always close)."""
    pads = {}
    i = 0
    while i + 1 < len(body):
        l, h = body[i], body[i + 1]
        pad = (h & 0xF0) >> 4
        raw = ((h & 0x0F) << 8) | l
        if pad < 16:
            pads[pad] = raw / 4095.0
        i += 2
    return pads


# ---------------------------------------------------------------------------
# Report 0x80 OUT: pad LEDs. 16 pads x 3 bytes RGB (0-127/channel).
# No remap needed - pad_idx here must match the pad_idx from parse_report20.
# See PROTOCOL.md for why (it's not as obvious as it sounds).
# ---------------------------------------------------------------------------

def set_pad_led(buf48, pad_idx, r, g, b):
    off = pad_idx * 3
    buf48[off] = r & 0x7F
    buf48[off + 1] = g & 0x7F
    buf48[off + 2] = b & 0x7F


def send_pad_leds(fd, buf48):
    os.write(fd, bytes([0x80]) + bytes(buf48))


def send_group_leds(fd, buf56):
    os.write(fd, bytes([0x81]) + bytes(buf56))


def send_other_button_leds(fd, buf31):
    os.write(fd, bytes([0x82]) + bytes(buf31))


# ---------------------------------------------------------------------------
# Feature reports 0xF8 / 0xF9: display contrast/brightness + flags.
# These go through HIDIOCGFEATURE/HIDIOCSFEATURE, not plain read()/write() -
# easiest via the `hid` pip package (ctypes bindings for hidapi), not os.read.
# ---------------------------------------------------------------------------

def get_display_feature(hid_device, display_report_id=0xF8):
    """hid_device: an open `hid.Device` (pip package `hid`, not os.open)."""
    return hid_device.get_feature_report(display_report_id, 16)


def set_display_contrast(hid_device, display_report_id, contrast, brightness, flags=0x00):
    buf = bytes([
        display_report_id,
        0x00, 0x01, 0x40, 0x00,  # read-only fields (256x64 self-description etc.), echoed back
        0x01, 0x01, 0x00,
        contrast & 0xFF, brightness & 0xFF, flags & 0xFF,
    ])
    hid_device.send_feature_report(buf)
