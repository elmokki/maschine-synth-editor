#!/usr/bin/env python3
"""Read/set display contrast+brightness via HID Feature reports (0xF8/0xF9).
These need actual Feature-report ioctls, not plain read()/write() - this
uses the `hid` pip package (ctypes bindings for hidapi) for that, which is
why this script doesn't use common.open_device() like the others.

    pip install hid

Usage: python3 feature_report.py [get|set CONTRAST BRIGHTNESS]
"""
import sys

import hid

VID, PID = 0x17CC, 0x1140


def main():
    d = hid.Device(VID, PID)
    print("opened:", d.manufacturer, d.product)

    if len(sys.argv) >= 2 and sys.argv[1] == "set":
        contrast = int(sys.argv[2]) if len(sys.argv) > 2 else 64
        brightness = int(sys.argv[3]) if len(sys.argv) > 3 else 50
        for report_id in (0xF8, 0xF9):
            buf = bytes([
                report_id, 0x00, 0x01, 0x40, 0x00, 0x01, 0x01, 0x00,
                contrast & 0xFF, brightness & 0xFF, 0x00,
            ])
            d.send_feature_report(buf)
        print(f"set contrast={contrast} brightness={brightness} on both displays")
    else:
        for report_id in (0xF8, 0xF9):
            data = d.get_feature_report(report_id, 16)
            print(f"0x{report_id:02X}: {data.hex()}")


if __name__ == "__main__":
    main()
