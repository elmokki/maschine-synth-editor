#!/usr/bin/env python3
"""JV-880 patches for src/synth_jv880.c.

A patch file is one patch: 498 bytes as in the Patch Mode Temporary Patch
(common 34 + 4 tones x 116, manual p. 226-227), named "NNN Name". This
writes data/jv880/000 Init, a plain patch to start from: tone 1 only, the
first internal wave through an open low-pass filter.

    python3 tools/jv880_import.py
"""
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "data", "jv880")


def common():
    c = [0] * 0x22
    c[0x00:0x0C] = list(b"Init Patch  ")
    c[0x0D] = 4        # reverb HALL1
    c[0x0E] = 32       # reverb level
    c[0x0F] = 64       # reverb time
    c[0x11] = 0        # chorus 1
    c[0x13] = 64       # chorus depth
    c[0x14] = 64       # chorus rate
    c[0x18] = 127      # patch level
    c[0x19] = 64       # patch pan centre
    c[0x1A] = 62       # bend down -2
    c[0x1B] = 2        # bend up +2
    c[0x1F] = 1        # portamento mode NORMAL
    return c


def tone(on):
    t = [0] * 0x74
    t[0x03] = on       # tone switch
    t[0x07] = 127      # velocity range upper
    t[0x08] = t[0x09] = 1   # volume sw, hold-1 sw
    for i in range(0x0A, 0x22, 2):
        t[i + 1] = 64  # modulation / aftertouch / expression: off, sense 0
    for lfo in (0x22, 0x2D):
        t[lfo + 1] = 2     # offset 0
        t[lfo + 3] = 64    # rate
        t[lfo + 8] = 64    # pitch depth 0
        t[lfo + 9] = t[lfo + 10] = 64
    t[0x38] = 64       # coarse 0
    t[0x39] = 64       # fine 0
    t[0x3B] = 12       # pitch key follow +100
    t[0x3C] = 64       # P-ENV velocity level sense 0
    t[0x3D] = t[0x3E] = t[0x3F] = 7   # time senses 0
    t[0x40] = 64       # P-ENV depth 0
    for lv in (0x42, 0x44, 0x46, 0x48):
        t[lv] = 64
    t[0x49] = 1        # LPF
    t[0x4A] = 127      # cutoff open
    t[0x4D] = 5        # cutoff key follow 0
    t[0x4F] = 64
    t[0x50] = t[0x51] = t[0x52] = 7
    t[0x53] = 64       # TVF-ENV depth 0
    t[0x55] = t[0x57] = t[0x59] = 127
    t[0x5C] = 127      # level
    t[0x5D] = 7        # level key follow 0
    t[0x5E] = 4        # pan centre (64 = 04 00)
    t[0x60] = 7
    t[0x65] = 100      # TVA velocity level sense
    t[0x66] = t[0x67] = t[0x68] = 7
    t[0x6A] = t[0x6C] = t[0x6E] = 127   # TVA levels
    t[0x6F] = 20       # release time
    t[0x70] = 127      # dry
    return t


def init_patch():
    p = common()
    for n in range(4):
        p += tone(1 if n == 0 else 0)
    assert len(p) == 0x22 + 4 * 0x74
    return bytes(p)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "000 Init"), "wb").write(init_patch())
    print("data/jv880/000 Init")
