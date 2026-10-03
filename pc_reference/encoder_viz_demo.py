#!/usr/bin/env python3
"""Live demo: draw a pie-chart + bar-graph on display 0 that follows
touch-encoder 1 in real time. Mostly useful as an integration smoke test
(reads report 0x01, writes report 0xE0 continuously) and a reminder that
fast display animation shows real ghosting on this hardware (see
docs/PROTOCOL.md) - that's a panel limitation, not a bug here.

Turn touch-encoder 1 (leftmost, above the screens) while this runs.
"""
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from common import (  # noqa: E402
    new_framebuffer,
    open_device,
    parse_report01,
    send_framebuffer,
    set_pixel,
)


def draw_rect_outline(fb, x0, y0, x1, y1):
    for x in range(x0, x1 + 1):
        set_pixel(fb, x, y0)
        set_pixel(fb, x, y1)
    for y in range(y0, y1 + 1):
        set_pixel(fb, x0, y)
        set_pixel(fb, x1, y)


def draw_bar(fb, x0, y0, x1, y1, val):
    draw_rect_outline(fb, x0, y0, x1, y1)
    fillw = int((x1 - x0 - 2) * val)
    for x in range(x0 + 2, x0 + 2 + fillw):
        for y in range(y0 + 2, y1 - 1):
            set_pixel(fb, x, y, True)


def draw_pie(fb, cx, cy, r, val):
    ang_max = val * 360.0
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx, dy = x - cx, y - cy
            if math.hypot(dx, dy) <= r:
                ang = math.degrees(math.atan2(dx, -dy)) % 360  # 0=up, clockwise
                if ang <= ang_max:
                    set_pixel(fb, x, y, True)


if __name__ == "__main__":
    fd = open_device()
    prev_fb = new_framebuffer()
    send_framebuffer(fd, prev_fb, 0xE0)

    last_val = None
    last_send = 0.0
    print("Turn touch-encoder 1. Ctrl+C to stop.")
    try:
        while True:
            buf = os.read(fd, 256)
            if not buf or buf[0] != 0x01:
                continue
            _buttons, _main_enc, touch_encs = parse_report01(buf[1:25])
            raw = touch_encs[0]
            val = max(0.0, min(1.0, raw / 999.0))
            if val == last_val:
                continue
            now = time.time()
            if now - last_send < 0.08:
                continue
            last_send = now
            last_val = val

            fb = new_framebuffer()
            draw_pie(fb, 32, 32, 24, val)
            draw_bar(fb, 70, 20, 250, 44, val)
            send_framebuffer(fd, fb, 0xE0, prev_fb)
            prev_fb = fb
            print(f"encoder1 raw={raw} val={val:.2f}")
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
