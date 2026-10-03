#!/usr/bin/env python3
"""Render a simple synth-editor-style two-line readout (param name + value)
to both displays, using Pillow + a bundled TTF font. Static - no need to
watch it run, just look at the device afterward.

Needs: pip install pillow  (a DejaVuSans.ttf is bundled with the `pymikro`
package if you have that installed too; otherwise point FONT_PATH at any
.ttf on your system, e.g. /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
from common import (  # noqa: E402
    DISPLAY_HEIGHT,
    DISPLAY_WIDTH,
    image_to_framebuffer,
    open_device,
    send_framebuffer,
)

FONT_PATH = os.environ.get(
    "MASCHINE_FONT",
    os.path.expanduser(
        "~/.local/lib/python3.14/site-packages/pymikro/DejaVuSans.ttf"
    ),
)


def make_readout(name, value):
    img = Image.new("1", (DISPLAY_WIDTH, DISPLAY_HEIGHT), 0)
    draw = ImageDraw.Draw(img)
    font_small = ImageFont.truetype(FONT_PATH, 14)
    font_big = ImageFont.truetype(FONT_PATH, 22)
    draw.text((6, 4), name, font=font_small, fill=1)
    draw.text((6, 22), value, font=font_big, fill=1)
    # Note: keep any border/right-aligned content off column 255 - it's
    # physically not visible on this unit (see docs/PROTOCOL.md).
    draw.rectangle((1, 1, DISPLAY_WIDTH - 2, DISPLAY_HEIGHT - 2), outline=1)
    return img


if __name__ == "__main__":
    fd = open_device()
    send_framebuffer(fd, image_to_framebuffer(make_readout("OSC1 WAVE", "SAWTOOTH")), 0xE0)
    send_framebuffer(fd, image_to_framebuffer(make_readout("CUTOFF", "72%")), 0xE1)
    print("sent text to both displays")
    os.close(fd)
