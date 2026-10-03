#!/usr/bin/env python3
"""Light each of the 16 pad LED slots in turn, one at a time, to visually
verify the pad_idx<->physical pad mapping (should go straight 0->15 with
no remap, per docs/PROTOCOL.md). Takes ~45s total."""
import sys
import time
import os

sys.path.insert(0, os.path.dirname(__file__))
from common import open_device, send_pad_leds  # noqa: E402

fd = open_device()

send_pad_leds(fd, bytearray(48))
time.sleep(0.5)

for slot in range(16):
    buf = bytearray(48)
    off = slot * 3
    buf[off:off + 3] = (127, 127, 127)
    send_pad_leds(fd, buf)
    print(f"slot {slot} lit")
    time.sleep(2.5)
    send_pad_leds(fd, bytearray(48))
    time.sleep(0.2)

print("done, all off")
os.close(fd)
