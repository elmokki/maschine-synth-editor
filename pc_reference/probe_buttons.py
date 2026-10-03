#!/usr/bin/env python3
"""Print button presses/releases by name as they happen. See
docs/PROTOCOL.md (report 0x01) for the bit map and its caveats (two
separate left/right arrow pairs, byte 6 unmapped, etc). Ctrl+C to stop."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from common import BUTTON_NAMES, open_device, parse_report01  # noqa: E402

fd = open_device(read_write=False)
prev = [False] * 56

print("Listening for button presses. Ctrl+C to stop.")
try:
    while True:
        buf = os.read(fd, 256)
        if not buf or buf[0] != 0x01:
            continue
        buttons, _main_enc, _touch_encs = parse_report01(buf[1:25])
        for i in range(56):
            if buttons[i] and not prev[i]:
                name = BUTTON_NAMES[i] or f"bit{i}(unmapped)"
                print(f"{name}  PRESSED")
            elif prev[i] and not buttons[i]:
                name = BUTTON_NAMES[i] or f"bit{i}(unmapped)"
                print(f"{name}  released")
        prev = buttons
except KeyboardInterrupt:
    pass
finally:
    os.close(fd)
