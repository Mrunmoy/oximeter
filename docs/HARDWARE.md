# OxiNode Hardware

> **First light achieved on 2026-04-25** (commit `da1dd3c`). The pin map below
> is no longer hypothetical — it is the wiring that produced a stable
> SpO2 = 98 % reading on the bench. The "Confirm before soldering" section is
> kept for anyone bringing up a *different* breakout — silkscreen variants
> in the MAX30102 module market are real, and matching against your specific
> board is still mandatory before powering it on.

## What this document is

The pinout, wiring, and power notes for OxiNode v1. Two host boards are
supported: Waveshare RP2040-Zero (priority — see
[`ARCHITECTURE.md`](ARCHITECTURE.md#threading-model--rp2040)) and Waveshare
ESP32-S3-Zero (phase 2). Both wire to the same MAX30102 breakout. Pin
allocations here are canonical — `firmware/<target>/include/board/pins.hpp`
must match this document. For *why* these pins, see
[`DESIGN.md`](DESIGN.md#d-06--ic-100-khz-default-400-khz-reserved-for-later).

---

## 1. Confirm before soldering

The MAX30102 breakouts on AliExpress, Amazon, and the various module shops
ship under at least four pin orderings under nearly identical board outlines.
**Before any solder touches the board:**

1. Read the silkscreen on *your* breakout, top side.
2. Match it to one of the variants in §3 below.
3. If your silkscreen does not match any variant, **stop** — do not assume.
   Trace VIN to the LDO input, GND to the largest copper pour, and the I²C
   pair via continuity to the MAX30102's pins 4 and 5.
4. Confirm the breakout's onboard LDO is in fact populated. Some clones
   omit it and require 1.8 V at VIN.

A wrong VIN connection on a clone with a missing LDO will brick the sensor
silently — the I²C interface keeps responding with garbage until the part
finally dies hours later.

> **Known PCB defect on a subset of GY-MAX30102 batches.** Many cheap
> "GY-MAX30102" modules tie the SDA / SCL / INT pull-ups to the **internal
> 1.8 V rail** (the chip's `VDD` digital supply) instead of the host's
> 3.3 V. On a 3.3 V I²C master this gives marginal logic levels and
> intermittent NACKs; on a 5 V master it is just broken. The fix is a
> trace-cut and a wire jumper to the 3.3 V rail. Reedpaper has photos
> and a step-by-step:
> <https://reedpaper.wordpress.com/2018/08/22/pulse-oximeter-max30100-max30102-how-to-fix-wrong-board/>.
> If your board reads PART_ID = 0x15 cleanly, drains the FIFO without
> errors, and the IRQ fires consistently, your batch already has the
> pull-ups in the right place — leave it alone. If you see flaky I²C
> at 100 kHz, this is the first thing to check.

---

## 2. RP2040-Zero — Waveshare

Waveshare RP2040-Zero is a USB-C castellated minimum-feature RP2040 board.
On-chip resources used by OxiNode v1: I²C0, GPIO IRQ, USB-CDC. The board
exposes 20 GPIO on the castellated edges and four more on the underside
pads.

### Board features (relevant subset)

| Feature | Detail |
|---------|--------|
| MCU | RP2040, dual Cortex-M0+ @ 125 MHz, 264 KB SRAM, 2 MB QSPI flash |
| USB | USB-C, native USB 1.1 PHY (used as USB-CDC by OxiNode) |
| Buttons | `BOOT` (held during plug-in for UF2 mass-storage mode), `RESET` |
| Onboard LED | Single WS2812 RGB on `GP16` (not used by OxiNode v1; available for status) |
| Power output | `3V3` pin is a 3.3 V regulator output, sourced from USB 5 V via the on-board LDO. ~300 mA available. |
| Castellated edge GPIO | `GP0`–`GP15`, `GP26`–`GP29` (analog-capable) |
| Underside pads | `GP16`, `GP17`, `GP18`, `GP19`, `GP20`, `GP21`, `GP22`, `GP23` (varies by silkscreen revision) |

### I²C0 capable pin pairs on RP2040

The RP2040 routes I²C0 to several pin pairs. We use the canonical
edge-accessible pair:

| SDA | SCL | Pinmux | Edge-accessible on RP2040-Zero? |
|-----|-----|--------|-------------------------------|
| `GP0` | `GP1` | I²C0 | yes |
| **`GP4`** | **`GP5`** | **I²C0** | **yes — chosen** |
| `GP8` | `GP9` | I²C0 | yes |
| `GP12` | `GP13` | I²C0 | yes |

`GP4`/`GP5` is chosen because:
- Both are on the same edge of the board, simplifying wiring.
- They are physically adjacent, allowing a 4-wire ribbon (`SDA`, `SCL`,
  `INT`, `GND`) to come off one side.
- They are not multiplexed against any of the buttons or LED.
- `GP6` is the next pin over, giving us `INT` adjacent to `SCL`.

### Verified wiring on the bench

The bring-up board exposes two extra silkscreen-labelled pads beyond the
canonical four:

| MAX30102 pad | RP2040-Zero pad | Role |
|--------------|-----------------|------|
| VIN | 3V3 (OUT) | sensor power, 3V3 jumper on breakout shorted |
| GND | GND | ground |
| SDA | GP4 | I²C0 SDA |
| SCL | GP5 | I²C0 SCL |
| INT | GP6 | open-drain interrupt — drives the RP2040 GPIO IRQ |
| RD | GP7 | **vestigial** — see callout below; firmware leaves GP7 as input, no contention |
| IRQ / IRD | GP8 | **vestigial** — see callout below; firmware leaves GP8 as input |

> **Why are `RD` and `IRD` (sometimes silkscreened `IRQ`) on the breakout
> at all?** They are *vestigial labels copied from the MAX30100 PCB
> layout.* The MAX30100 (predecessor chip) brought its LED cathodes out
> on dedicated pins for an external sense-resistor topology. The
> MAX30102 *integrates the LED drivers and the LEDs themselves into the
> 14-OESIP optical package* — so the chip pins these pads route to are
> all explicitly **N.C.** in the datasheet:
>
> > *"No Connection. Connect to PCB pad for mechanical stability."*
> > — MAX30102 datasheet rev 1, §Pin Description, p.8 (pins 1, 5, 6, 7, 8, 14)
>
> When PCB designers retrofitted the older MAX30100 module layout for
> the MAX30102 die, they kept the silkscreen text. The traces under
> those pads still go to chip pins, but those chip pins are not
> connected to anything inside the silicon. Continuity-check them
> against ground or the 3.3 V rail with a multimeter and you will see
> open in both directions — they are floating.
>
> **Practical implication.** Wiring `RD` → `GP7` and `IRD` → `GP8` is
> harmless: the GPIOs see open inputs, the firmware never reads or
> writes them, and there is no contention. Don't try to "use" them for
> anything sensor-related; there is nothing to use. The only real
> interrupt the chip exposes is the single `INT` pin (chip pin 13 →
> breakout `INT` → `GP6`).

### Final wiring — RP2040-Zero ↔ MAX30102

| RP2040-Zero pin | Direction | MAX30102 breakout pin | Notes |
|-----------------|-----------|-----------------------|-------|
| `3V3` (OUT) | → | `VIN` | Breakout LDO accepts 3.3–5 V; we feed 3.3 V from the RP2040's regulator. ~30 mA at idle, peaks ~100 mA during LED pulses — well within the LDO's 300 mA budget. |
| `GND` | — | `GND` | |
| `GP4` | ↔ | `SDA` | I²C0 SDA, 100 kHz; 4.7 kΩ pull-up on the breakout. |
| `GP5` | → | `SCL` | I²C0 SCL, 100 kHz; 4.7 kΩ pull-up on the breakout. |
| `GP6` | ← | `INT` | Open-drain, active low. **Internal pull-up enabled in firmware** (`firmware/rp2040/hal/PicoIntPin.cpp`). Falling-edge IRQ on FIFO_A_FULL. |
| — | — | `IRD` | Leave floating. Vestigial label routed to chip N.C. pin (see §2 callout). |
| — | — | `RD` | Leave floating. Vestigial label routed to chip N.C. pin (see §2 callout). |

`pins.hpp` for this target lives at
`firmware/rp2040/include/board/pins.hpp` and is the single source of truth —
if it disagrees with this document, fix the document or fix the header, but
never both at once.

### Optional — SSD1306 OLED dashboard on I²C1

A 0.96" 128×64 SSD1306 OLED (I²C variant, address 0x3C) renders a live
HR / SpO₂ dashboard so the device is useful standalone, without a host
client attached. The OLED runs on **I²C1** — completely separate from
the MAX30102's I²C0 — so its full-frame refresh (~25 ms at 400 kHz)
cannot back-pressure the sensor drain.

| RP2040-Zero pin | Direction | SSD1306 pin | Notes |
|-----------------|-----------|-------------|-------|
| `3V3` (OUT) | → | `VDD` / `VCC` | Most modules accept 3.3–5 V via on-module charge-pump. |
| `GND` | — | `GND` | |
| `GP14` | ↔ | `SDA` | I²C1 SDA, 400 kHz fast-mode. Internal pull-up enabled in firmware. |
| `GP15` | → | `SCL` | I²C1 SCL, 400 kHz. Internal pull-up enabled in firmware. |

Pin pair rationale: GP14/GP15 is the cleanest free I²C1 pair on the
RP2040-Zero pin map. The other valid I²C1 SDA/SCL pair (GP6/GP7) is
unusable here because GP6 is the MAX30102 INT line. GP4–GP9 are
already wired to the sensor or its vestigial pads.

The OLED is **optional**. If the panel is unplugged (or its breakout
fails to ack on the bus), `Ssd1306::init()` returns non-zero, the
firmware emits one `oled_init_failed` log line, and continues normally
— the JSON-Lines link to a host client is the source of truth and
must keep flowing regardless.

---

## 3. MAX30102 breakout variants

The MAX30102 die is the same regardless of breakout vendor; the pin
*ordering* on the carrier is not. The four common variants you will
encounter:

### 3a. GY-MAX30102 — 7-pin (most common)

Silkscreen, top side, left to right:

```
GND   VIN   SCL   SDA   INT   IRD   RD
```

- VIN accepts 3.3–5 V via on-board LDO.
- 4.7 kΩ pull-ups to the LDO output on SDA and SCL.
- INT is pulled to the same rail through 4.7 kΩ on some revisions, floating
  on others — assume floating and enable the host pull-up.
- IRD and RD are vestigial silkscreen labels routed to chip N.C. pins (see §2 callout); do not connect.

This is the variant assumed by the canonical wiring in §2.

### 3b. GY-MAX30102 — 7-pin, alternate ordering

Silkscreen, top side, left to right:

```
VIN   GND   SDA   SCL   INT   IRD   RD
```

Identical electrically; **VIN and GND swap positions**. If you wire the §2
table to this board, you put 3.3 V into GND and GND into the LDO input. The
LDO will hold 0 V on the output, the MAX30102 will not respond, and the
breakout will run warm — that warmth is the LDO bleeding off your supply
through its protection diode.

### 3c. Adafruit MAX30102 — 5-pin

Silkscreen:

```
VIN   GND   SCL   SDA   INT
```

- No `IRD` / `RD` pins broken out.
- Includes level-shifters for 5 V I²C masters (relevant for Arduino but
  not for our 3.3 V parts).
- Pull-ups are present and correctly sized for 3.3 V operation.

Wire the same as §2; the IRD and RD rows do not apply.

### 3d. SparkFun MAX30101 (NOT MAX30102)

Visually similar; **silkscreen reads MAX30101** and the pin ordering is
yet another variant. The MAX30101 is a different die with three LEDs
(red, IR, green) and is **not register-compatible** with the MAX30102 init
sequence in [`lib/max3010x/src/Max30102.cpp`](../lib/max3010x/src/Max30102.cpp).
If your sensor is a MAX30101, OxiNode v1 will not drive it correctly.

---

## 4. ESP32-S3-Zero — Waveshare (phase 2)

Waveshare ESP32-S3-Zero is a USB-C castellated ESP32-S3 minimum board.
ESP32-S3-Zero firmware is skeleton-only in v1; this section is the wiring
plan for when phase 2 starts.

### Board features (relevant subset)

| Feature | Detail |
|---------|--------|
| MCU | ESP32-S3 dual-core LX7 @ 240 MHz, 512 KB internal SRAM, 4 MB flash, 2 MB PSRAM |
| USB | USB-C, native USB-Serial-JTAG (no FTDI / CP210x). Default boot uses GPIO19/GPIO20 for D-/D+. |
| Buttons | `BOOT` (GPIO0), `RESET` |
| Onboard LED | WS2812 RGB on `GPIO21` |
| Strapping pins | GPIO0, GPIO3, GPIO45, GPIO46 — avoid for sensor signals. |
| Reserved (SPI flash) | GPIO26–GPIO32 — not exposed. |
| Castellated edge GPIO | GPIO0–GPIO14 plus GPIO33–GPIO48 (subset; varies by edge) |

### I²C on ESP32-S3

Unlike RP2040, the ESP32-S3's I²C peripherals are pin-multiplexed via the
GPIO matrix and can route to almost any GPIO. There is no "default I²C0
pin pair" the way there is on RP2040; we choose freely.

### Final wiring — ESP32-S3-Zero ↔ MAX30102

| ESP32-S3-Zero pin | Direction | MAX30102 breakout pin | Notes |
|-------------------|-----------|-----------------------|-------|
| `3V3` | → | `VIN` | On-board LDO output, ~500 mA budget. |
| `GND` | — | `GND` | |
| `GPIO8` | ↔ | `SDA` | I²C master SDA. Not a strapping pin. |
| `GPIO9` | → | `SCL` | I²C master SCL. |
| `GPIO10` | ← | `INT` | GPIO interrupt input, internal pull-up enabled in firmware. |
| — | — | `IRD` | Leave floating. |
| — | — | `RD` | Leave floating. |

`GPIO8`–`GPIO10` are deliberately picked away from the strapping pins, the
USB-Serial-JTAG pins, the WS2812 LED on `GPIO21`, and the SPI flash range.
They are physically adjacent on the castellated edge for clean wiring.

---

## 5. Power budget

| Source | Sink | Worst-case current | Mitigation |
|--------|------|--------------------|------------|
| USB 5 V → host MCU LDO → 3V3 OUT | MAX30102 (LED pulses + ADC) | ~100 mA peak | Both target boards' LDOs are rated ≥ 300 mA; comfortable margin. |
| 3V3 OUT | I²C pull-ups (4.7 kΩ × 2) | ~1.4 mA | Negligible. |
| 3V3 OUT | INT pull-up (internal, ~50 kΩ) | ~70 µA | Negligible. |

If you extend the wiring beyond ~100 mm or run multiple sensors from one
host, you'll need to revisit the pull-up sizing on SDA/SCL — at 100 kHz
the existing 4.7 kΩ is fine for a 30 pF lumped capacitance, but rises in
proportion to wire length.

---

## 6. Mechanical notes

- The MAX30102 photodiode is on the same side as the silkscreen text. The
  user's finger goes against that side.
- The two LED windows on the MAX30102 die are separated by a small black
  plastic occluder; if you 3D-print a finger cradle, do not block this
  occluder with translucent plastic.
- Avoid ambient daylight on the sensor when first bringing up the firmware
  — the IR channel saturates easily, and an unexpected DC offset will
  confuse the bandpass filter in
  [`lib/max3010x/src/HrDetector.cpp`](../lib/max3010x/src/HrDetector.cpp)
  for the first few seconds.

---

## 7. INT pin behaviour — read this before debugging "no samples"

The MAX30102 INT pin is **open-drain, active low**. The breakout does not
pull it up to VCC reliably — some boards include a 4.7 kΩ pull-up,
others leave the pad bare. OxiNode firmware enables the host MCU's
internal pull-up on the INT GPIO unconditionally:

```cpp
// firmware/rp2040/hal/PicoIntPin.cpp  (excerpt)
gpio_init(kIntPin);
gpio_set_dir(kIntPin, GPIO_IN);
gpio_pull_up(kIntPin);                       // critical — INT is open-drain
gpio_set_irq_enabled_with_callback(kIntPin,
                                   GPIO_IRQ_EDGE_FALL,
                                   true,
                                   &intIsrTrampoline);
```

If you skip the pull-up, the line floats, you see spurious IRQ edges
every time the wiring jiggles, and the data task wakes to read an empty
FIFO. Symptom: many ISR firings, zero samples in the link queue.
