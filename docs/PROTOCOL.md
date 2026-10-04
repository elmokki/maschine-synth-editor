# Native Instruments Maschine Mk2 (full-size) — reverse-engineered protocol

Device: `17cc:1140` "Native Instruments Maschine Controller MK2" (full-size, not Mikro).
Everything in this document was derived and confirmed on **real hardware** this session,
using raw `hidraw` reads/writes from Python/Rust — no USB packet capture (Wireshark/usbmon)
was needed or used. Where something is a cross-reference from existing open-source projects
(`shaduzlabs/cabl`, `SnovaxZ/MaschineMK2_linux`) rather than our own hardware test, it's marked
as such — several of those assumptions turned out to be wrong for this exact unit/firmware
revision and had to be corrected empirically. Treat any *unconfirmed* note the same way: verify
on hardware before trusting it for anything real (e.g. firmware).

## Why this approach (Native protocol, not the device's own "MIDI mode")

The device has two firmware personalities:
- **Native/Controller protocol** (default) — raw vendor HID, full feature access (per-pad RGB,
  both displays, velocity-sensitive pads, all buttons/encoders). This is what NI's own software
  and this document use.
- **"MIDI mode"** (toggled on some NI devices via a button combo, e.g. SHIFT+CONTROL per NI's
  docs) — fixed note/CC mapping, dimmed LEDs only, no display control. **On this specific unit,
  SHIFT+CONTROL did nothing observable** — likely because that dimming/mode-switch behavior is
  actually driven by NI's Windows driver (NIHIA), not a standalone firmware toggle. We don't use
  or need this mode at all; see the DIN MIDI section below for why.

## USB topology

4 interfaces in one composite device (`lsusb -v -d 17cc:1140`):

| IF | Class | Purpose | Endpoints |
|----|-------|---------|-----------|
| 0 | Audio Control | IAD header for IF0+IF1 | none |
| 1 | Audio/MIDIStreaming | **Standard USB-MIDI class interface** | 0x81 IN / 0x01 OUT, bulk, 512B |
| 2 | HID | Everything in this doc (control surface) | 0x82 IN / 0x02 OUT, **interrupt, 64B max packet** |
| 3 | DFU | Firmware update | none |

IF2 (HID) is what shows up as e.g. `/dev/hidraw6` on Linux (exact number varies by system —
match on `HID_ID=...17CCp00001140` via `udevadm`/sysfs, not a hardcoded path).

Despite the HID interface's small 64-byte max packet, a single `write()` of a 265-byte HID
output report is accepted and transmitted in full — the kernel/USB stack chunks it into multiple
packets transparently. No special handling needed on the host side for this.

## 5-pin DIN MIDI — works out of the box, no mode-switch needed

The USB-MIDI class interface (IF1) has its External MIDI_IN_JACK ("Maschine MK2 Ext Input") and
External MIDI_OUT_JACK ("Maschine MK2 Ext Output") wired directly to the Embedded jacks that talk
to the host over standard USB-MIDI. **Confirmed bidirectionally on hardware**: a plain MIDI
note sent to this interface (e.g. via `amidi -p hw:X,0,0 -S "90 3C 7F"`) comes out the physical
DIN OUT jack, and MIDI fed into the physical DIN IN jack arrives at the same USB-MIDI port on
the host. This is completely independent of the HID interface (IF2) and needs zero custom
protocol — **TinyUSB's `usb_host` MIDI class driver can talk to this directly**, as a separate,
simple code path from all the HID/Native-protocol work below.

## HID Report Reference (interface 2)

All reports are "numbered reports": the first byte written/read is the report ID.

### `0x01` IN — buttons + encoders (24 bytes after the report-ID byte)

| Bytes (0-indexed within the 24-byte body) | Content |
|---|---|
| 0-6 (7 bytes, 56 bits) | Button states, see table below. Bit `i` → `body[i/8] & (1<<(i%8))`. |
| 7 | **Main (center) encoder.** Low nibble = rotation counter, confirmed wrapping 0-15, ±1 per detent (it has real mechanical clicks). High nibble: untested, meaning unknown. |
| 8-23 (16 bytes) | **8 endless encoders** above the two displays (no detents, no end stops), 16-bit little-endian each, consecutive pairs: (8,9), (10,11), (12,13), (14,15), (16,17), (18,19), (20,21), (22,23). Each value is a **free-running position counter, 0-999, that wraps** both ways (999→0 when turning clockwise, 0→999 counter-clockwise; confirmed on hardware, no saturation/plateau at the ends). The absolute value has no physical meaning — use it **relatively**: `delta = cur - prev`, and when `|delta| > 500` correct for wrap by subtracting or adding 1000. One full physical turn ≈ **1300 units** (≈1.3 counter wraps; measured roughly as ~3887 units over ~3 hand-counted turns, so treat as ±10%). The device only reports changes of **≥4 units** (it has its own deadband, so the smallest delta seen was ±4). A small backward step (−4…−13) shows up when you stop or re-grip mid-turn; filter it if it bothers you. Pair order matches physical left-to-right position above the displays (not independently verified which pair NI itself calls "encoder 1" vs "8"). |

**⚠️ Common mistake when reading this from a raw `hidraw` buffer:** byte 0 of the raw read is
the report ID (`0x01`), so body byte 7 (main encoder) is `buf[8]`, and touch-encoder 1 is
`buf[9]`/`buf[10]`. Forgetting this +1 offset and reading `buf[8]`/`buf[9]` directly produces
wildly out-of-range values (seen up to 65535) that look like a protocol failure but are just an
indexing bug in test code. Hit this once already — check this first if encoder values look insane.

**Button bit map** (fully confirmed on hardware, byte-aligned, no scrambling — unlike pads/LEDs
below, `cabl`'s assumed bit order needed no correction here):

| Byte | Bits (LSB→MSB) |
|---|---|
| 0 | Display button 1, 2, 3, 4, 5, 6, 7, 8 (left→right, the soft-keys above/below the screens) |
| 1 | Control, Step, Browse, Sampling, Arrow-Left, Arrow-Right, All, Auto |
| 2 | Volume, Swing, Tempo, Arrow-Left #2, Arrow-Right #2, Enter, Note Repeat, *(bit 7 unused)* |
| 3 | Group A, B, C, D, E, F, G, H |
| 4 | Restart, Step-Left, Step-Right, Grid, Play, Rec, Erase, Shift |
| 5 | Scene, Pattern, Pad Mode, Navigate, Duplicate, Select, Solo, Mute |
| 6 | *Unmapped.* Bit 0 read as always-high from the very start of every test (likely not a real button, or a status/heartbeat bit); bits 1-7 never tested. |

Note: there are genuinely **two separate left/right arrow pairs** (byte1 bits 4/5, and byte2
bits 3/4) — likely one is page-turning and the other fine cursor navigation; which is which
wasn't disambiguated, just that they're independent controls.

Not yet mapped: push-click on the touch-encoders or main encoder (if they even have one —
untested).

### `0x20` IN — pads (32 × 16-bit slots, i.e. 64 bytes after the report-ID byte)

The pad identity is encoded **in the data itself**, not implied by slot position — this is the
single biggest gotcha on this device. For each 16-bit little-endian slot (`l` = low byte, `h` =
high byte):

```
pad      = (h & 0xF0) >> 4      # 0-15, which physical pad this slot is reporting
pressure = (((h & 0x0F) << 8) | l) / 4095.0   # 0.0-1.0
```

Each physical pad reports in **two of the 32 slots per read** (redundant/double-sampled — treat
both identically, it's not an error). A naive implementation that reads `pad = slot_position`
instead of extracting the nibble produces exactly the "LED lights the wrong/adjacent pad"
symptom.

**Logical pad index ↔ physical pad, and the LED output slot ↔ physical pad mapping
(`0x80` below) use NO remap at all relative to each other** — i.e. `pad_idx = (h&0xF0)>>4`
directly, and writing LED slot `pad_idx` lights that same physical pad correctly. This was *not*
obvious going in:
- The hardware's own pad-sense-matrix nibble order and its pad-LED-matrix slot order are each
  individually "scrambled" relative to simple physical left-to-right/top-to-bottom numbering —
  confirmed by two **independent, isolated, single-variable** hardware tests:
  - Input: pressing physical pads 1→16 in order gave raw nibbles `12,13,14,15, 8,9,10,11,
    4,5,6,7, 0,1,2,3` in that order.
  - Output: a clean sweep lighting output slots 0→15 one at a time (all else off) produced
    physical pads lighting in order `13,14,15,16, 9,10,11,12, 5,6,7,8, 1,2,3,4`.
  - Both of these are the *same* permutation `σ = [12,13,14,15,8,9,10,11,4,5,6,7,0,1,2,3]`
    (indexed 0-15), which happens to be its own inverse (two disjoint 4-block swaps). Since the
    *same* σ governs both directions, they cancel out algebraically — hence "no remap needed" is
    correct, but only because both sides happen to use the same scramble. **Do not assume this
    symmetry on other devices/firmwares** — it was verified, not assumed, and it took three wrong
    attempts (remap on output only → confirmed vertical mirror; remap on both sides → same
    mirror again; no remap at all → correct) to land on it.
- `cabl`'s own `Led` enum encodes exactly this σ, which is where the table was first *found*
  (cross-reference), but cabl's code applies it asymmetrically in a way that doesn't transfer
  directly to this codebase's conventions — it had to be re-derived and verified independently
  for the actual input→output round-trip to come out right.

**Analog cross-talk between pad sense channels is real**, confirmed on hardware: pressing one
pad firmly (raw ~1400+) makes neighboring/related channels read ~200-750 simultaneously. Any
debounce threshold needs clear margin above that — see the state-machine bug below for why this
matters more than it sounds.

**If porting a pad-debounce/threshold state machine from an existing reference implementation,
check every state transition explicitly re-checks the real threshold.** `SnovaxZ`'s
`MaschinePad::pressure_val()` had a state (`PressedBelowThreshold`) that could be entered by
tiny cross-talk bleed and then **never** checked the threshold again until pressure returned to
*exactly* 0.0 — a pad stuck in that state would never register "Pressed" again even at raw
value 4095/4095 (confirmed on hardware — this wasted a lot of the session chasing phantom
"hardware defect"/"wrong mapping" theories before the actual one-line state-machine bug was
found). Don't reproduce that bug in new code.

### `0x80` OUT — pad LEDs (48 bytes after report-ID: 16 pads × 3 bytes RGB, 0-127/channel)

`offset = 1 + pad_idx*3` (relative to the write buffer including the report-ID byte at offset 0).
See the pad section above for `pad_idx` — no remap needed.

### `0x81` OUT — group buttons + transport LEDs (56 bytes after report-ID)

- Bytes 0-47: 8 Group buttons (A→H) × 6 bytes each = **two separate RGB triples per group
  button** (first triple at `group*6 + 0..2`, second at `group*6 + 3..5`). Confirmed both
  triples independently light the correct group button, in A→H order. What the *second* triple
  is for in practice (selected-state? has-content indicator?) wasn't determined — only that it's
  a second, independently-settable RGB value.
- Bytes 48-55: 8 transport buttons, mono (single brightness byte each): Restart, Step-Left,
  Step-Right, Grid, Play, Rec, Erase, Shift (assumed in the same order as the input bit map —
  only spot-checked, not individually confirmed by name).

### `0x82` OUT — other button LEDs (31 bytes after report-ID, mono only)

Covers the remaining named buttons (display buttons, Control/Step/Browse cluster,
Scene/Pattern/etc. cluster, Volume/Swing/Tempo/Enter/NoteRepeat). Only spot-checked a handful
of slots (0,1,2,28,29,30), not an exhaustive 1-by-1 confirmation like the input bit map got —
redo properly before relying on this for real UI feedback.

### `0xE0` / `0xE1` OUT — displays (265 bytes per write: 9-byte header + 256 data bytes)

Each display is **256×64 px, 1 bit per pixel**, row-major, MSB-first within each byte
(`byte[(stride*y)+(x>>3)] & (0x80 >> (x&7))`), `stride = 32` bytes/row, 2048 bytes total per
display, sent as **8 chunks of 256 bytes** (one chunk = 8 rows × 32 bytes):

```
header = [ reportID,        # 0xE0 (display 1) or 0xE1 (display 2)
           0x00, 0x00,      # unused
           chunk*8, 0x00,   # Y row offset (chunk 0..7 → rows 0,8,16,...,56)
           0x20, 0x00,      # width-related constant, always 0x20
           0x08, 0x00 ]     # height-related constant, always 0x08
packet = header + <256 bytes of pixel data for this chunk>
```

Confirmed on hardware via a clean chunk-by-chunk sweep (alternating on/off chunks produced
exactly 4 evenly-spaced stripes spanning the *full* height — confirms all 8 chunks correctly
span the full 64 rows, evenly). An earlier mistake (reusing some assumptions from `SnovaxZ`'s
buggy original code: a 512-byte data buffer instead of 256, and the `0x08`/`0x20` header bytes
swapped) looked like "nothing happens" for a long time — it wasn't a transport/endpoint problem,
just wrong byte values in the header/size. **Found by testing one single chunk in isolation with
a raw Python `os.write()`, not by debugging the full application** — that isolation technique is
what actually cracked this (and the pad mapping, and the button map) after a lot of confusing,
contradictory signals trying to infer things through a full running application instead.

**Known hardware limitation, precisely diagnosed:** the rightmost pixel column (**x=255**) is
not visible — physically cropped by the panel/bezel, confirmed asymmetric (left edge x=0
displays fine). **Treat usable width as x=0..254 (255px)**, don't rely on column 255 for borders,
right-aligned text, etc.

**Ghosting/smearing during fast animated updates is real and NOT fixable from software**: tried
(a) sending each chunk twice back-to-back, (b) slowing the minimum update interval from 30ms to
80ms — neither changed it. This is a passive-matrix-LCD-style physical response-time limit of
the panel itself. Acceptable for a synth-editor UI where the display only updates when a knob is
actually turned, not continuously animated.

**Text rendering**: use `Pillow` (`PIL.ImageDraw` + `ImageFont.truetype`) to render onto a
256×64 mode `"1"` image, then convert to the bitmap above pixel-by-pixel. Confirmed readable on
hardware at multiple font sizes. See `pc_reference/text_demo.py`.

### `0xF8` / `0xF9` FEATURE — display config (11 bytes incl. report ID, via `HIDIOCGFEATURE`/
`HIDIOCSFEATURE` — i.e. `get_feature_report`/`send_feature_report`, *not* plain `read()`/`write()`)

Decoded fields (byte offsets within the 11-byte report):
- Bytes 1-2, 3-4 (16-bit each): read-only, reported as `256` and `64` — **the display's own
  width/height self-description**, nicely confirms the 256×64 canvas size independently.
- Bytes 5-7: read-only, read as `1, 1, 0` on this unit — purpose unknown.
- Bytes 8-9: **writable, confirmed to be contrast/brightness** (0-100 range per descriptor,
  default read as 64 and 50). Setting both to 0 visibly turned the screen off; restoring 64/50
  turned it back on. Confirmed on hardware.
- Byte 10: 8 boolean flags, writable per the descriptor, but writing `0xFF` had no observable
  effect and didn't persist in a readback — likely unimplemented/reserved on this firmware.

## Open/unverified items (don't assume, re-test before relying on these)

- Main encoder's high nibble (byte 7 bits 4-7) — never exercised.
- Which physical encoder (left-to-right) corresponds to which byte pair was inferred from
  test order, not independently labeled.
- Push-click on encoders, if any.
- Byte 6 bits 1-7 of the button bitfield (bit 0 is a probable red herring, see button table).
- Exact meaning of the group buttons' *second* RGB triple.
- Full 1-by-1 confirmation of `0x82` and the transport half of `0x81` (only spot-checked).

## Tools

See `pc_reference/` for the Python scripts (all self-written for this project, not copied from
any GPL source) that were used to find and verify everything above — same "isolated, one HID
field at a time" technique throughout. Point them at your own system's `/dev/hidrawN` node
(find it via `udevadm info -q property -n /dev/hidrawN | grep 17CC`).

Reference projects consulted while reverse-engineering (none of their code is copied into this
repo — `cabl` and `MaschineMK2_linux` are GPL/MIT licensed respectively and were only read, not
vendored):
- [`shaduzlabs/cabl`](https://github.com/shaduzlabs/cabl) (C++, MIT) — most useful single
  source for the display chunk format and pad nibble-extraction logic, despite being unmaintained
  since 2018 and getting several exact byte values wrong for this unit (see corrections above).
- [`SnovaxZ/MaschineMK2_linux`](https://github.com/SnovaxZ/MaschineMK2_linux) (Rust, GPL-3.0) —
  used as a live, buildable test harness against real hardware; its bugs (display buffer size,
  pad debounce state machine) are documented above specifically so they aren't reintroduced here.
