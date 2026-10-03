#!/usr/bin/env python3
"""Print encoder changes (main encoder + 8 touch-encoders) as they happen.
See docs/PROTOCOL.md (report 0x01) - touch-encoders are absolute position
(0-999ish), not deltas; the main encoder is a wrapping 0-15 click counter.
Ctrl+C to stop."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from common import open_device, parse_report01  # noqa: E402

fd = open_device(read_write=False)
prev_main = None
prev_touch = [None] * 8

print("Listening for encoder changes. Ctrl+C to stop.")
try:
    while True:
        buf = os.read(fd, 256)
        if not buf or buf[0] != 0x01:
            continue
        _buttons, main_enc, touch_encs = parse_report01(buf[1:25])

        if main_enc != prev_main:
            print(f"main encoder = {main_enc}")
            prev_main = main_enc

        for i in range(8):
            if touch_encs[i] != prev_touch[i]:
                print(f"touch-encoder {i+1}: {touch_encs[i]}")
                prev_touch[i] = touch_encs[i]
except KeyboardInterrupt:
    pass
finally:
    os.close(fd)
