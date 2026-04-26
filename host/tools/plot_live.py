#!/usr/bin/env python3
"""
Live JSON-Lines visualizer for the OxiNode RP2040 firmware.

Reads newline-delimited JSON objects from /dev/ttyACM0 (or another
serial device) and animates a five-region matplotlib figure:

    ┌─────────────────────────────────────────────┐
    │ Header strip  HR / SpO₂ / PI big readouts   │
    │               + LOCKED / ACQUIRING pill     │
    ├──────────────────────────┬──────────────────┤
    │ IR + RED raw counts      │ HR (BPM)         │
    │                          │  + median + IQR  │
    ├──────────────────────────┼──────────────────┤
    │ IR demeaned (cardiac AC) │ SpO₂ (%)         │
    │                          │  + median + IQR  │
    └──────────────────────────┴──────────────────┘

Header readouts are sourced from the firmware's `hr` / `spo2`
fields (already median-of-N debounced inside HrDetector — see
docs/SOFTWARE.md §4 and the D-17 design entry). Perfusion index
is computed on the host from a rolling window of IR samples.

The HR/SpO2 panels overlay rolling 5-th/95-th percentile bands
and a median line so an outlier blip is visible *as* a blip
rather than masquerading as a stable reading. The "LOCKED /
ACQUIRING" pill is the at-a-glance signal: green LOCKED when
the latest valid HR has held within a tight band for several
seconds, orange ACQUIRING otherwise.

Usage (inside the Nix dev shell):

    python3 host/tools/plot_live.py             # uses /dev/ttyACM0
    python3 host/tools/plot_live.py --port /dev/ttyACM1
    python3 host/tools/plot_live.py --window 600 --fps 10

The script does not write to the device — the firmware is in
JSON-Lines mode by default (see docs/PROTOCOL.md and SOFTWARE.md §6).
Press Ctrl+C or close the window to exit.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
import threading
import time
from typing import Deque

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np
import serial


# Minimum number of valid HR samples before LOCKED can fire.
LOCK_MIN_SAMPLES = 25 * 5    # ~5 s of valid HR
# Maximum |HR - median| over the last LOCK_MIN_SAMPLES samples for LOCKED.
LOCK_TOLERANCE_BPM = 4
# IQR band percentiles. 5/95 catches obvious outliers without being so
# tight that normal rolling jitter looks alarming.
BAND_LOW_PCT = 5.0
BAND_HIGH_PCT = 95.0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="/dev/ttyACM0",
                   help="serial device (default: %(default)s)")
    p.add_argument("--baud", type=int, default=115200,
                   help="ignored on USB-CDC but accepted for symmetry (default: %(default)s)")
    p.add_argument("--window", type=int, default=500,
                   help="number of samples to keep in the rolling view (default: %(default)s)")
    p.add_argument("--fps", type=float, default=15.0,
                   help="animation frame rate (default: %(default)s)")
    return p.parse_args()


# ── shared rolling buffers ─────────────────────────────────────────────────
class Buffers:
    """Five parallel deques. The reader thread appends, the animation
    thread reads under a lock for atomic snapshots."""

    def __init__(self, capacity: int) -> None:
        self.capacity = capacity
        self.t: Deque[float] = collections.deque(maxlen=capacity)
        self.ir: Deque[int] = collections.deque(maxlen=capacity)
        self.red: Deque[int] = collections.deque(maxlen=capacity)
        self.hr: Deque[int] = collections.deque(maxlen=capacity)
        self.spo2: Deque[int] = collections.deque(maxlen=capacity)
        self.lock = threading.Lock()
        self.last_alive: dict | None = None

    def push_sample(self, t: float, ir: int, red: int,
                    hr: int, spo2: int) -> None:
        with self.lock:
            self.t.append(t)
            self.ir.append(ir)
            self.red.append(red)
            self.hr.append(hr)
            self.spo2.append(spo2)

    def snapshot(self) -> tuple[np.ndarray, ...]:
        with self.lock:
            return (np.fromiter(self.t,    dtype=np.float64),
                    np.fromiter(self.ir,   dtype=np.int64),
                    np.fromiter(self.red,  dtype=np.int64),
                    np.fromiter(self.hr,   dtype=np.int64),
                    np.fromiter(self.spo2, dtype=np.int64))


# ── reader thread ──────────────────────────────────────────────────────────
def reader_loop(port: str, baud: int, buffers: Buffers,
                stop_evt: threading.Event) -> None:
    """Open the serial port and feed buffers. Reconnects on read errors —
    BOOTSEL trick / unplugs / replugs are tolerated."""
    while not stop_evt.is_set():
        try:
            with serial.Serial(port, baud, timeout=0.5) as ser:
                ser.reset_input_buffer()
                start_t = time.monotonic()
                for raw in ser:
                    if stop_evt.is_set():
                        return
                    line = raw.decode("ascii", errors="ignore").strip()
                    if not line or line[0] != "{":
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if "alive" in obj:
                        buffers.last_alive = obj
                        continue
                    if "ir" in obj and "red" in obj:
                        # 't' is firmware uptime in ms; we use host monotonic
                        # for plot x-axis so reconnects don't jump backward.
                        buffers.push_sample(
                            time.monotonic() - start_t,
                            int(obj.get("ir", 0)),
                            int(obj.get("red", 0)),
                            int(obj.get("hr", 0) or 0),
                            int(obj.get("spo2", 0) or 0),
                        )
        except (serial.SerialException, OSError) as e:
            print(f"[reader] {e}; retrying in 1s", file=sys.stderr)
            time.sleep(1.0)


# ── derived signals ────────────────────────────────────────────────────────
def perfusion_index(ir: np.ndarray) -> float:
    """Maxim AN6845 §"Perfusion Index": PI = (AC_pp / DC) × 100. We use
    peak-to-peak over the rolling window for AC, and the window mean
    for DC. Returns 0.0 if the window is too short or DC is zero."""
    if ir.size < 25 or ir.mean() <= 0:
        return 0.0
    ac_pp = float(ir.max() - ir.min())
    return (ac_pp / float(ir.mean())) * 100.0


def lock_state(hr: np.ndarray) -> tuple[str, str]:
    """Return ("LOCKED"|"ACQUIRING", colour) for the header pill.

    LOCKED: have at least LOCK_MIN_SAMPLES recent samples whose HR is
    valid and within ±LOCK_TOLERANCE_BPM of the window median. This
    matches the "did the median-of-N-output finish settling?" check
    a clinician implicitly does before trusting the displayed BPM."""
    valid = hr[hr > 0]
    if valid.size < LOCK_MIN_SAMPLES:
        return ("ACQUIRING", "#e09040")
    recent = valid[-LOCK_MIN_SAMPLES:]
    med = float(np.median(recent))
    if float(np.abs(recent - med).max()) <= LOCK_TOLERANCE_BPM:
        return ("LOCKED", "#40c060")
    return ("ACQUIRING", "#e09040")


# ── plot ───────────────────────────────────────────────────────────────────
def main() -> int:
    args = parse_args()
    buffers = Buffers(args.window)
    stop_evt = threading.Event()

    rx = threading.Thread(target=reader_loop,
                          args=(args.port, args.baud, buffers, stop_evt),
                          daemon=True)
    rx.start()

    plt.style.use("dark_background")
    fig = plt.figure(figsize=(13, 8))
    fig.canvas.manager.set_window_title(f"OxiNode live — {args.port}")

    # ── Header strip (gridspec row 0) ──────────────────────────────────
    # Left half: big HR/SpO2/PI numbers. Right half: lock pill.
    gs = fig.add_gridspec(3, 2, height_ratios=[1.1, 2.5, 2.5],
                          hspace=0.45, wspace=0.18,
                          left=0.06, right=0.97, top=0.97, bottom=0.06)
    ax_hdr = fig.add_subplot(gs[0, :])
    ax_hdr.set_axis_off()

    txt_hr = ax_hdr.text(0.02, 0.55, "HR --",  fontsize=34, color="#ff5050",
                         family="monospace", weight="bold",
                         transform=ax_hdr.transAxes, va="center")
    txt_sp = ax_hdr.text(0.22, 0.55, "SpO₂ --%", fontsize=28, color="#50a0ff",
                         family="monospace", weight="bold",
                         transform=ax_hdr.transAxes, va="center")
    txt_pi = ax_hdr.text(0.45, 0.55, "PI --%",  fontsize=24, color="#aaa",
                         family="monospace",
                         transform=ax_hdr.transAxes, va="center")
    txt_pill_bg = ax_hdr.text(0.78, 0.55, "  ACQUIRING  ",
                              fontsize=20, color="white",
                              family="monospace", weight="bold",
                              transform=ax_hdr.transAxes, va="center", ha="center",
                              bbox=dict(boxstyle="round,pad=0.45",
                                        fc="#e09040", ec="none"))
    txt_status = ax_hdr.text(0.02, 0.05,
                             "waiting for samples…",
                             fontsize=9, color="#888",
                             family="monospace",
                             transform=ax_hdr.transAxes, va="bottom")

    # ── Top-left: raw IR + RED ────────────────────────────────────────
    ax_raw = fig.add_subplot(gs[1, 0])
    ax_raw.set_title("IR / RED (raw 18-bit ADC counts)")
    ax_raw.set_ylabel("counts")
    ln_ir, = ax_raw.plot([], [], lw=1.0, color="#ff5050", label="IR (880 nm)")
    ln_red, = ax_raw.plot([], [], lw=1.0, color="#50a0ff", label="RED (660 nm)")
    ax_raw.axhline(150_000, ls=":", lw=0.7, color="#888",
                   label="AN6845 finger floor 150 K")
    ax_raw.axhline(196_608, ls=":", lw=0.7, color="#aaa",
                   label="UG6409 ¾-FS 197 K")
    ax_raw.legend(loc="lower right", fontsize=8)
    ax_raw.grid(True, alpha=0.2)

    # ── Bottom-left: IR demeaned (cardiac AC) ─────────────────────────
    ax_ac = fig.add_subplot(gs[2, 0])
    ax_ac.set_title("IR demeaned (cardiac AC waveform)")
    ax_ac.set_xlabel("time (s)")
    ax_ac.set_ylabel("counts (Δ from mean)")
    ln_ac, = ax_ac.plot([], [], lw=1.0, color="#ffaa50")
    ax_ac.axhline(0, ls="-", lw=0.4, color="#666")
    ax_ac.grid(True, alpha=0.2)

    # ── Top-right: HR + median + IQR band ─────────────────────────────
    ax_hr = fig.add_subplot(gs[1, 1])
    ax_hr.set_title("Heart rate (BPM) — sample, median, 5-95 % band")
    ax_hr.set_ylabel("BPM")
    ax_hr.set_ylim(30, 180)
    ln_hr,     = ax_hr.plot([], [], lw=1.2, color="#ff8080",
                            drawstyle="steps-post", alpha=0.8, label="sample")
    ln_hr_med, = ax_hr.plot([], [], lw=2.0, color="#ff5050",
                            label="median (rolling)")
    poly_hr = ax_hr.fill_between([0, 1], 0, 0, color="#ff5050",
                                 alpha=0.15, label="5–95 %")
    ax_hr.axhspan(60, 100, alpha=0.05, color="#50ff50",
                  label="resting normal")
    ax_hr.legend(loc="lower right", fontsize=8)
    ax_hr.grid(True, alpha=0.2)

    # ── Bottom-right: SpO2 + median + IQR band ────────────────────────
    ax_sp = fig.add_subplot(gs[2, 1])
    ax_sp.set_title("SpO₂ (%) — sample, median, 5-95 % band")
    ax_sp.set_xlabel("time (s)")
    ax_sp.set_ylabel("%")
    ax_sp.set_ylim(80, 100)
    ln_sp,     = ax_sp.plot([], [], lw=1.2, color="#80c0ff",
                            drawstyle="steps-post", alpha=0.8, label="sample")
    ln_sp_med, = ax_sp.plot([], [], lw=2.0, color="#50a0ff",
                            label="median (rolling)")
    poly_sp = ax_sp.fill_between([0, 1], 0, 0, color="#50a0ff",
                                 alpha=0.15, label="5–95 %")
    ax_sp.axhspan(95, 100, alpha=0.05, color="#50ff50",
                  label="healthy adult")
    ax_sp.legend(loc="lower right", fontsize=8)
    ax_sp.grid(True, alpha=0.2)

    def update(_frame):
        nonlocal poly_hr, poly_sp

        t, ir, red, hr, sp = buffers.snapshot()
        if len(t) < 2:
            return ()

        # ── Raw + AC panels ─────────────────────────────────────────
        ln_ir.set_data(t, ir)
        ln_red.set_data(t, red)

        ir_demean = ir - ir.mean()
        ln_ac.set_data(t, ir_demean)

        for ax in (ax_raw, ax_ac, ax_hr, ax_sp):
            ax.set_xlim(t[0], t[-1] + 0.1)
        ir_lo, ir_hi = int(ir.min()), int(ir.max())
        red_lo, red_hi = int(red.min()), int(red.max())
        lo = min(ir_lo, red_lo) - 2000
        hi = max(ir_hi, red_hi) + 2000
        ax_raw.set_ylim(lo, hi)
        ac_lim = max(2000, int(np.abs(ir_demean).max()) + 500)
        ax_ac.set_ylim(-ac_lim, ac_lim)

        # ── HR + SpO2 plots (only valid samples) ────────────────────
        hr_mask = hr > 0
        sp_mask = sp > 0
        ln_hr.set_data(t[hr_mask], hr[hr_mask])
        ln_sp.set_data(t[sp_mask], sp[sp_mask])

        # Rolling median + percentile band over the *whole* window of
        # valid samples. fill_between cannot be `set_data`-updated;
        # we have to remove and recreate the PolyCollection.
        poly_hr.remove()
        poly_sp.remove()
        if hr_mask.any():
            t_hr = t[hr_mask]
            v_hr = hr[hr_mask].astype(np.float64)
            med = float(np.median(v_hr))
            lo_b = float(np.percentile(v_hr, BAND_LOW_PCT))
            hi_b = float(np.percentile(v_hr, BAND_HIGH_PCT))
            ln_hr_med.set_data([t_hr[0], t_hr[-1]], [med, med])
            poly_hr = ax_hr.fill_between([t_hr[0], t_hr[-1]],
                                         lo_b, hi_b,
                                         color="#ff5050", alpha=0.18)
        else:
            ln_hr_med.set_data([], [])
            poly_hr = ax_hr.fill_between([0, 1], 0, 0, color="#ff5050",
                                         alpha=0.18)

        if sp_mask.any():
            t_sp = t[sp_mask]
            v_sp = sp[sp_mask].astype(np.float64)
            med = float(np.median(v_sp))
            lo_b = float(np.percentile(v_sp, BAND_LOW_PCT))
            hi_b = float(np.percentile(v_sp, BAND_HIGH_PCT))
            ln_sp_med.set_data([t_sp[0], t_sp[-1]], [med, med])
            poly_sp = ax_sp.fill_between([t_sp[0], t_sp[-1]],
                                         lo_b, hi_b,
                                         color="#50a0ff", alpha=0.18)
        else:
            ln_sp_med.set_data([], [])
            poly_sp = ax_sp.fill_between([0, 1], 0, 0, color="#50a0ff",
                                         alpha=0.18)

        # ── Header strip ────────────────────────────────────────────
        last_hr = int(hr[-1]) if hr_mask.any() else 0
        last_sp = int(sp[-1]) if sp_mask.any() else 0
        pi = perfusion_index(ir)
        state, colour = lock_state(hr)

        txt_hr.set_text(f"HR {last_hr:>3}" if last_hr else "HR  --")
        txt_sp.set_text(f"SpO₂ {last_sp:>2}%" if last_sp else "SpO₂ --%")
        txt_pi.set_text(f"PI {pi:4.1f}%")
        txt_pill_bg.set_text(f"  {state}  ")
        txt_pill_bg.get_bbox_patch().set_facecolor(colour)

        alive = buffers.last_alive or {}
        status = (f"samples={len(t):4d}  "
                  f"IR={ir[-1]:6d}  RED={red[-1]:6d}  "
                  f"alive: edges={alive.get('edges', '-'):>5}  "
                  f"drain={alive.get('drain', '-')}  "
                  f"ring_hwm={alive.get('ring_hwm', '-')}  "
                  f"cfg_crc={alive.get('cfg_crc', '-')}  "
                  f"n={alive.get('cfg_crc_n', '-')}")
        txt_status.set_text(status)

        return ()

    interval_ms = max(1, int(1000.0 / args.fps))
    # blit=False because the bands (PolyCollection) are recreated each
    # frame and matplotlib's blit cache can't track that.
    ani = animation.FuncAnimation(fig, update, interval=interval_ms,
                                  blit=False, cache_frame_data=False)

    try:
        plt.show()
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        rx.join(timeout=2.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
