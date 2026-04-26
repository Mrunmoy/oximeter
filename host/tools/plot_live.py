#!/usr/bin/env python3
"""
Live JSON-Lines visualizer for the OxiNode RP2040 firmware.

Reads newline-delimited JSON objects from /dev/ttyACM0 (or another
serial device) and animates a four-panel matplotlib figure:

    ┌──────────────────────────┬───────────────────────┐
    │ IR + RED raw counts      │ HR (BPM)              │
    ├──────────────────────────┼───────────────────────┤
    │ IR demeaned (cardiac AC) │ SpO₂ (%)              │
    └──────────────────────────┴───────────────────────┘

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
    """Four parallel deques. The reader thread appends, the animation
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
    fig, axes = plt.subplots(2, 2, figsize=(12, 7))
    fig.canvas.manager.set_window_title(f"OxiNode live — {args.port}")
    fig.suptitle("OxiNode live — JSON-Lines @ /dev/ttyACM0", fontsize=11)

    # Top-left: raw IR + RED
    ax_raw = axes[0, 0]
    ax_raw.set_title("IR / RED (raw 18-bit ADC counts)")
    ax_raw.set_ylabel("counts")
    ln_ir,  = ax_raw.plot([], [], lw=1.0, color="#ff5050", label="IR (880 nm)")
    ln_red, = ax_raw.plot([], [], lw=1.0, color="#50a0ff", label="RED (660 nm)")
    ax_raw.axhline(150_000, ls=":", lw=0.7, color="#888",
                   label="AN6845 finger floor 150 K")
    ax_raw.axhline(196_608, ls=":", lw=0.7, color="#aaa",
                   label="UG6409 ¾-FS 197 K")
    ax_raw.legend(loc="lower right", fontsize=8)
    ax_raw.grid(True, alpha=0.2)

    # Bottom-left: IR demeaned (the cardiac AC waveform — what HrDetector sees)
    ax_ac = axes[1, 0]
    ax_ac.set_title("IR demeaned (cardiac AC waveform)")
    ax_ac.set_xlabel("time (s)")
    ax_ac.set_ylabel("counts (Δ from mean)")
    ln_ac, = ax_ac.plot([], [], lw=1.0, color="#ffaa50")
    ax_ac.axhline(0, ls="-", lw=0.4, color="#666")
    ax_ac.grid(True, alpha=0.2)

    # Top-right: HR
    ax_hr = axes[0, 1]
    ax_hr.set_title("Heart rate (BPM)")
    ax_hr.set_ylabel("BPM")
    ax_hr.set_ylim(30, 180)
    ln_hr, = ax_hr.plot([], [], lw=1.5, color="#ff5050", drawstyle="steps-post")
    ax_hr.axhspan(60, 100, alpha=0.07, color="#50ff50",
                  label="resting normal")
    ax_hr.legend(loc="lower right", fontsize=8)
    ax_hr.grid(True, alpha=0.2)

    # Bottom-right: SpO2
    ax_sp = axes[1, 1]
    ax_sp.set_title("SpO₂ (%)")
    ax_sp.set_xlabel("time (s)")
    ax_sp.set_ylabel("%")
    ax_sp.set_ylim(80, 100)
    ln_sp, = ax_sp.plot([], [], lw=1.5, color="#50a0ff", drawstyle="steps-post")
    ax_sp.axhspan(95, 100, alpha=0.07, color="#50ff50",
                  label="healthy adult")
    ax_sp.legend(loc="lower right", fontsize=8)
    ax_sp.grid(True, alpha=0.2)

    txt_status = fig.text(0.01, 0.005, "waiting for samples…",
                          fontsize=9, color="#aaaaaa", family="monospace")

    fig.tight_layout(rect=(0, 0.02, 1, 0.97))

    def update(_frame):
        t, ir, red, hr, sp = buffers.snapshot()
        if len(t) < 2:
            return ln_ir, ln_red, ln_ac, ln_hr, ln_sp, txt_status

        ln_ir.set_data(t, ir)
        ln_red.set_data(t, red)

        ir_demean = ir - ir.mean()
        ln_ac.set_data(t, ir_demean)

        # HR / SpO2 — only show valid (non-zero) values
        hr_mask = hr > 0
        ln_hr.set_data(t[hr_mask], hr[hr_mask])

        sp_mask = sp > 0
        ln_sp.set_data(t[sp_mask], sp[sp_mask])

        # Auto-scale x to the rolling window; y for the two raw panels.
        for ax in (ax_raw, ax_ac, ax_hr, ax_sp):
            ax.set_xlim(t[0], t[-1] + 0.1)
        ir_lo, ir_hi = int(ir.min()), int(ir.max())
        red_lo, red_hi = int(red.min()), int(red.max())
        lo = min(ir_lo, red_lo) - 2000
        hi = max(ir_hi, red_hi) + 2000
        ax_raw.set_ylim(lo, hi)
        ac_lim = max(2000, int(np.abs(ir_demean).max()) + 500)
        ax_ac.set_ylim(-ac_lim, ac_lim)

        # Status bar
        last_hr = int(hr[-1]) if hr_mask.any() else 0
        last_sp = int(sp[-1]) if sp_mask.any() else 0
        alive = buffers.last_alive or {}
        status = (f"samples={len(t):4d}  "
                  f"IR={ir[-1]:6d}  RED={red[-1]:6d}  "
                  f"HR={last_hr:3d}  SpO2={last_sp:3d}  "
                  f"alive: edges={alive.get('edges', '-'):>4}  "
                  f"int1={alive.get('int1', '-')}  "
                  f"drain={alive.get('drain', '-')}")
        txt_status.set_text(status)

        return ln_ir, ln_red, ln_ac, ln_hr, ln_sp, txt_status

    interval_ms = max(1, int(1000.0 / args.fps))
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
