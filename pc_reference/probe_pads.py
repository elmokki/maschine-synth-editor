#!/usr/bin/env python3
"""Print pad presses/releases as they happen. See docs/PROTOCOL.md (report
0x20) for the format. Run and tap pads; Ctrl+C to stop."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from common import open_device, parse_report20  # noqa: E402

THRESHOLD = 0.10  # fraction of full scale; tune against your own hardware's
                   # observed cross-talk ceiling (~0.18 was seen on one unit)

fd = open_device(read_write=False)
state = {}  # pad_idx -> bool currently "pressed"

print("Listening for pad presses. Ctrl+C to stop.")
try:
    while True:
        buf = os.read(fd, 256)
        if not buf or buf[0] != 0x20:
            continue
        pads = parse_report20(buf[1:])
        for pad_idx, pressure in pads.items():
            was = state.get(pad_idx, False)
            now = pressure > THRESHOLD
            if now and not was:
                print(f"pad {pad_idx:2d}  PRESSED   pressure={pressure:.3f}")
            elif was and not now:
                print(f"pad {pad_idx:2d}  released")
            state[pad_idx] = now
except KeyboardInterrupt:
    pass
finally:
    os.close(fd)
