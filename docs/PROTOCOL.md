# OxiNode Wire Protocol — RP2040 USB-CDC

## What this document is

The bytes-on-the-wire spec for the link between the OxiNode RP2040 firmware
and a host computer over USB-CDC. Two coexisting modes are defined:
**JSON-Lines** (default, eyeballable in any serial terminal) and **binary
frames** (length-prefixed, CRC-protected, for the Python desktop client and
any future high-throughput consumer). Both modes carry the same logical
records — `(t, ir, red, hr, spo2)` samples plus periodic status. The mode is
controlled by simple ASCII commands. For the *why* behind the dual mode and
the CRC choice see
[`DESIGN.md`](DESIGN.md#d-02--json-lines-is-the-default-link-mode-binary-frames-are-opt-in)
and
[`DESIGN.md`](DESIGN.md#d-03--crc-16ccitt-false-for-the-binary-frame).
For the data-flow this protocol sits inside, see
[`ARCHITECTURE.md`](ARCHITECTURE.md#end-to-end-data-flow).

> **Compatibility rule.** The binary frame `TYPE` enum is **append-only**.
> Existing values are never repurposed. Any change to a payload's field
> layout requires a new `TYPE` *and* a bump to the `oxinode` version field
> in the schema-version line. Hosts that don't know a `TYPE` MUST skip the
> frame using the `LEN` field, never assume a length, never crash.

---

## 1. Link parameters

| Property | Value |
|----------|-------|
| Transport | USB-CDC (tinyusb on the device side) |
| Logical baud | Irrelevant — USB-CDC framing is packet-based; the host's `B115200` is decorative. The firmware ignores `cfMakeRaw` baud bits. |
| 8-N-1 / line discipline | Raw bytes, no flow control. Host should call `cfmakeraw()` and disable echo. |
| Endianness (binary mode) | Little-endian for all multi-byte integer fields. |
| Character encoding (JSON mode) | UTF-8, ASCII subset only — no escapes beyond `\\`, `\"`, `\n`. |

---

## 2. Boot sequence

On USB enumeration / DTR rise, the device emits a **single schema-version
line** in JSON-Lines mode, then begins streaming samples:

```jsonl
{"oxinode":"v1","mode":"jsonl"}
{"t":12345,"ir":123456,"red":98765,"hr":72,"spo2":98}
{"t":12365,"ir":123890,"red":98910,"hr":72,"spo2":98}
...
```

The schema-version line is the first newline-terminated record after
enumeration. A host that reads bytes before the first `\n` should discard
them (USB-CDC may flush stale buffers from before the device's last reset).

---

## 3. JSON-Lines mode (default)

### 3.1 Sample record

One line per sample. UTF-8, terminated with a single `\n` (0x0A). No
trailing whitespace, no embedded newlines, no `\r`.

```json
{"t":<u32 ms>,"ir":<u32>,"red":<u32>,"hr":<u8>,"spo2":<u8>}
```

| Field | Type | Range | Meaning |
|-------|------|-------|---------|
| `t` | u32 | 0 .. 4 294 967 295 | Monotonic milliseconds since device boot. Wraps every ~49.7 days. |
| `ir` | u32 | 0 .. 262 143 | IR photodiode ADC reading (18-bit value, transmitted as a u32 for forward-compat). |
| `red` | u32 | 0 .. 262 143 | Red photodiode ADC reading. |
| `hr` | u8 | 0 .. 250 | Heart rate in BPM, derived per [`lib/max3010x/src/HrDetector.cpp`](../lib/max3010x/src/HrDetector.cpp). `0` means "not yet locked." |
| `spo2` | u8 | 0 .. 100 | SpO2 percentage, derived per [`lib/max3010x/src/Spo2Algo.cpp`](../lib/max3010x/src/Spo2Algo.cpp). `0` means "not yet locked." |

### 3.2 Status record

Emitted on state transitions and on warning conditions. Distinguishable
from sample records by the absence of the `t` field at the top level and
the presence of `status`.

```json
{"status":"<word>","reason":"<sentence>"}
```

`status` is a short token from the table below; `reason` is a free-form
sentence intended for human readers in `picocom`.

| `status` token | Meaning |
|----------------|---------|
| `boot` | Device just enumerated; the schema-version line was the immediately prior line. |
| `sensor_ready` | MAX30102 init succeeded, IR/Red LED currents set. |
| `sensor_fault` | I²C transaction failed during init or runtime. `reason` will name the register. |
| `link_overrun` | Internal SPSC ring overflowed because the host hasn't been draining the CDC IN endpoint. Samples dropped. |
| `mode_switch` | Issued just before the device transitions to binary mode. |

### 3.3 Worked example

```
{"oxinode":"v1","mode":"jsonl"}
{"status":"sensor_ready","reason":"MAX30102 reset OK, RED=24mA IR=24mA"}
{"t":102,"ir":121580,"red":97342,"hr":0,"spo2":0}
{"t":122,"ir":121640,"red":97401,"hr":0,"spo2":0}
{"t":4242,"ir":127512,"red":102874,"hr":71,"spo2":97}
```

---

## 4. Mode switch

### 4.1 Host → device: `MODE BIN`

The host sends the literal seven-byte ASCII string followed by `\n`:

```
M O D E SP B I N \n     (0x4D 0x4F 0x44 0x45 0x20 0x42 0x49 0x4E 0x0A)
```

**Case-sensitive.** Anything else is ignored as an unknown command.

The device:

1. Finishes flushing any sample line currently in its CDC IN buffer.
2. Emits exactly one final JSON-Lines record:
   ```json
   {"oxinode":"v1","mode":"bin"}
   ```
3. Switches the link writer to the binary framer in
   `firmware/rp2040/link/BinaryFramer.cpp`.

After the trailing `\n` of the `mode:"bin"` line, every subsequent byte from
the device follows the binary frame layout in §5. Hosts MUST consume that
final JSON line before starting their binary-frame parser.

### 4.2 Device → host: ACK on STOP

To go back, see §6 (CTRL frame).

---

## 5. Binary frame layout

```
+------+--------+------+----------------+----------+
| STX  |  LEN   | TYPE |    PAYLOAD     |   CRC16  |
| 0xAA | u16 LE |  u8  |  variable      |  u16 LE  |
+------+--------+------+----------------+----------+
   1 B    2 B    1 B   LEN-1 bytes        2 B
```

| Field | Size | Meaning |
|-------|------|---------|
| `STX` | 1 byte | Start sentinel, fixed `0xAA`. Used for resync after a corrupted frame. |
| `LEN` | u16 LE | Number of bytes in `TYPE` + `PAYLOAD`. **Does not** include `STX`, `LEN`, or `CRC`. Always ≥ 1 (TYPE is mandatory). Hard cap: 1024 for v1. |
| `TYPE` | u8 | Frame type code, see §5.1. Append-only enum. |
| `PAYLOAD` | LEN − 1 bytes | TYPE-specific. See §5.2 onward. |
| `CRC16` | u16 LE | CRC-16/CCITT-FALSE over `LEN` ‖ `TYPE` ‖ `PAYLOAD`. See §7. |

### 5.1 TYPE codes

| Code | Direction | Name | Payload size | Description |
|------|-----------|------|--------------|-------------|
| `0x01` | device → host | `SAMPLE` | 14 B | One sample record (fields below). |
| `0x02` | device → host | `STATUS` | 4 B | Periodic device-state record. |
| `0x03` | device → host | `ACK` | 1 B | Acknowledgement of a received CTRL frame. |
| `0xFE` | host → device | `CTRL` | 1 B | Control command from the host. |

Values not listed are **reserved** and MUST NOT be sent. Receivers that
encounter an unknown TYPE MUST consume `LEN` bytes of payload + 2 bytes of
CRC, validate the CRC, and silently drop the frame (do not crash, do not
desync).

### 5.2 SAMPLE (TYPE = 0x01, payload = 14 B)

```
offset  size  field
   0     u32   t       (LE) — milliseconds since boot
   4     u32   ir      (LE) — IR ADC, 18-bit value in a u32
   8     u32   red     (LE) — Red ADC, 18-bit value in a u32
  12     u8    hr             — BPM, 0 = not locked
  13     u8    spo2           — percent, 0 = not locked
```

Total payload = 14 bytes. Total frame on the wire = 1 + 2 + 1 + 14 + 2 = 20 bytes.

### 5.3 STATUS (TYPE = 0x02, payload = 4 B)

```
offset  size  field
   0     u8    state           — see table below
   1     u8    fifo            — FIFO occupancy at last drain (0..32)
   2     i16   temp_q8_8 (LE)  — die temperature, Q8.8 fixed point in °C
```

`state`:

| Value | Name |
|-------|------|
| `0x00` | INIT |
| `0x01` | RUNNING |
| `0x02` | FAULT |
| `0x03` | OVERRUN |

### 5.4 ACK (TYPE = 0x03, payload = 1 B)

```
offset  size  field
   0     u8    result          — 0x00 = ok, 0x01 = bad CRC, 0x02 = unknown cmd
```

### 5.5 CTRL (TYPE = 0xFE, payload = 1 B, host → device only)

```
offset  size  field
   0     u8    cmd
```

| `cmd` | Name | Effect |
|-------|------|--------|
| `0x00` | `STOP_BIN` | Device replies with one ACK frame `result=0x00`, then reverts to JSON-Lines mode. The next JSON line emitted will be `{"oxinode":"v1","mode":"jsonl"}`. |
| `0x01` | `RESET_DSP` | Device clears HR and SpO2 lock state without re-initialising the sensor. Replies with ACK. |

---

## 6. Reverse mode switch (binary → JSON-Lines)

The host builds and sends the following frame:

```
AA  02 00  FE  00  <CRC16 LE>
│   │      │   │   │
│   │      │   └── cmd = STOP_BIN
│   │      └────── TYPE = CTRL
│   └────────────── LEN = 2  (TYPE + 1 byte payload)
└──────────────────  STX
```

CRC-16/CCITT-FALSE over `02 00 FE 00` is `0x5966`, so the encoded bytes
LE are `66 59`. Full frame on the wire (hex):

```
AA 02 00 FE 00 66 59
```

The device:

1. Validates the CRC. If it fails, replies `ACK(result=0x01)` and stays in
   binary mode.
2. If valid, replies `ACK(result=0x00)` (still as a binary frame).
3. Switches the link writer back to JSON-Lines and emits
   `{"oxinode":"v1","mode":"jsonl"}` followed by normal sample lines.

The host MUST consume the trailing ACK *before* resetting its parser to
JSON-Lines, because the ACK is the last byte the device emits in binary
mode.

---

## 7. CRC-16/CCITT-FALSE reference

Parameters (also known as `CRC-16/IBM-3740` or `CRC-16/AUTOSAR`):

| Parameter | Value |
|-----------|-------|
| Polynomial | `0x1021` |
| Init | `0xFFFF` |
| RefIn | false (no input bit reflection) |
| RefOut | false (no output bit reflection) |
| XorOut | `0x0000` |
| Check value | `0x29B1` (CRC of the ASCII string `"123456789"`) |

The check value is the standard regression test — a host implementation
that does not produce `0x29B1` for the byte sequence
`31 32 33 34 35 36 37 38 39` is broken and will reject every device frame.

### 7.1 C reference

```c
#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
        {
            crc = (crc & 0x8000u)
                ? (uint16_t)((crc << 1) ^ 0x1021u)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

The bit-shift loop above is what the firmware actually compiles to — see
`firmware/rp2040/link/BinaryFramer.cpp`. A 256-entry table is faster but
costs 512 bytes of flash and was not justified at the link rates we run.

### 7.2 Python reference

```python
def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

assert crc16_ccitt_false(b"123456789") == 0x29B1
```

This is the implementation `host/desktop_client/oxinode_protocol.py` ships
with — the assert at module import is the regression guard.

---

## 8. Resync rules (binary mode)

USB-CDC is mostly reliable, but a host that detaches mid-frame can leave a
parser misaligned. The recovery rule is:

1. Discard bytes until the next `0xAA` is seen.
2. Read `LEN` (2 bytes LE).
3. If `LEN == 0` or `LEN > 1024`, treat the `0xAA` as spurious, discard,
   continue from step 1.
4. Read `LEN + 2` more bytes (TYPE + PAYLOAD + CRC).
5. Compute CRC over `LEN ‖ TYPE ‖ PAYLOAD`, compare against received CRC.
6. On mismatch, discard the leading `0xAA` and continue from step 1.
7. On match, dispatch by `TYPE`.

`0xAA` is intentionally a value that does not appear at known-fixed offsets
of the SAMPLE payload (the smallest field is the BPM byte, which is bounded
to 0..250), but it is not impossible inside `ir` or `red`. Resync after
disturbance therefore relies on the CRC eventually telling truth from
coincidence, which it does within one or two frames in practice.

---

## 9. Versioning policy

The schema-version field is `"oxinode": "v1"`. This document and the
firmware in `firmware/rp2040/link/` are v1.

**TYPE enum is append-only.** Once a value is shipped — even in pre-release
firmware — it is reserved forever for the meaning documented here. New
record types take new TYPE codes, in the unallocated ranges:

| Range | Status |
|-------|--------|
| `0x00` | Reserved (forbidden — would collide with a stray null byte). |
| `0x01` .. `0x7F` | Device → host records. `0x01`, `0x02`, `0x03` allocated; rest available. |
| `0x80` .. `0xFD` | Reserved for future device → host records (e.g. die-temperature stream, raw FIFO dump). |
| `0xFE` | Host → device CTRL. |
| `0xFF` | Reserved (forbidden — would collide with a stray 0xFF idle byte). |

**Payload changes bump the version string.** If `SAMPLE` ever grows a
field, the schema-version line on connect becomes `"oxinode":"v2"`, and
the `TYPE` for the new SAMPLE is a new code (e.g. `0x04`). The old
`TYPE 0x01` continues to mean exactly what §5.2 says it means, forever, on
any v1-or-later firmware. Hosts that only speak v1 continue to work
against a v2 device by ignoring unknown TYPE codes per §5.1.

This rule is non-negotiable. The whole point of CRC framing with explicit
LEN is that we can ship new frame types without breaking old hosts; if we
let TYPE codes drift in meaning we throw that away.
