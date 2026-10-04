#!/usr/bin/env python3
"""Add files to the device's LittleFS without losing what's on it.

`pio run -t uploadfs` replaces the whole partition with data/, which would
wipe the user's own patches and state. This instead:
  1. reads the partition back from the device (also a backup),
  2. adds/replaces the given data/ subdirectories in that image,
  3. writes the merged image back - only with --write.

Run from the project root, with the PlatformIO Python (it has littlefs and
esptool), e.g.:
    ~/.platformio/penv/bin/python tools/lfs_merge.py plugiator/b4000
    ~/.platformio/penv/bin/python tools/lfs_merge.py plugiator/b4000 --write

Partition offset/size must match partitions.csv ("storage").
"""
import argparse
import os
import subprocess
import sys

from littlefs import LittleFS

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OFFSET, SIZE = 0x410000, 0xBF0000
ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")


def esptool(*args):
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", PORT,
                    "--baud", "921600", *args], check=True)


def mount(path):
    data = bytearray(open(path, "rb").read())
    fs = LittleFS(block_size=4096, block_count=len(data) // 4096, name_max=64, mount=False)
    fs.context.buffer[:] = data
    fs.mount()
    return fs


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+", help="data/ subdirectories to add, e.g. plugiator/b4000")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--work", default=".pio/lfs_merge", help="where backup/merged images go")
    ap.add_argument("--write", action="store_true", help="write the merged image to the device")
    a = ap.parse_args()
    PORT = a.port
    os.makedirs(a.work, exist_ok=True)
    backup = os.path.join(a.work, "lfs_backup.bin")
    merged = os.path.join(a.work, "lfs_merged.bin")

    esptool("read_flash", hex(OFFSET), hex(SIZE), backup)
    fs = mount(backup)
    for d in a.dirs:
        src = os.path.join(ROOT, "data", d)
        fs.makedirs("/" + d, exist_ok=True)
        names = sorted(os.listdir(src))
        for name in names:
            with fs.open(f"/{d}/{name}", "wb") as f:
                f.write(open(os.path.join(src, name), "rb").read())
        print(f"{d}: {len(names)} files")
    fs.unmount()
    open(merged, "wb").write(bytes(fs.context.buffer))
    check = mount(merged)
    print("merged image root:", check.listdir("/"))

    if a.write:
        esptool("write_flash", hex(OFFSET), merged)
    else:
        print(f"backup: {backup}\nmerged: {merged}\n(not written - add --write)")
