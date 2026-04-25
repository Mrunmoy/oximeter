"""Textual TUI for the OxiNode desktop client.

Two-pane layout:

* Left — large HR + SpO2 numbers, last sample timestamp, and a finger-detected
  indicator (true when the IR DC level is above a heuristic threshold or the
  most recent ``StatusEvent.state`` reports a finger).
* Right — a 30-second sparkline of HR.

Key bindings:

* ``b`` — switch to binary mode
* ``j`` — switch to JSON-Lines mode
* ``r`` — send CTRL RESET_DSP (binary mode only)
* ``q`` — quit
"""

from __future__ import annotations

import logging
from collections import deque
from typing import ClassVar, Final

from rich.align import Align
from rich.console import Group, RenderableType
from rich.panel import Panel
from rich.table import Table
from rich.text import Text
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal
from textual.reactive import reactive
from textual.widgets import Footer, Header, Static

from .link import AckEvent, BannerEvent, Link, LinkMode, Sample, StatusEvent

__all__ = ["OxinodeTui"]

_LOG: Final[logging.Logger] = logging.getLogger(__name__)

# IR DC heuristic for "finger detected" when no STATUS frame is available.
_IR_FINGER_THRESHOLD: Final[int] = 50_000
_HR_HISTORY_SECONDS: Final[float] = 30.0
_POLL_INTERVAL: Final[float] = 0.05


def _sparkline(samples: list[int], width: int = 40) -> str:
    """Unicode sparkline. Empty input returns a placeholder."""
    if not samples:
        return "—" * width
    blocks = " ▁▂▃▄▅▆▇█"
    lo = min(samples)
    hi = max(samples)
    span = max(hi - lo, 1)
    # Resample to ``width`` buckets (nearest-neighbour) so the line scales.
    if len(samples) >= width:
        step = len(samples) / width
        picks = [samples[min(int(i * step), len(samples) - 1)] for i in range(width)]
    else:
        picks = samples
    return "".join(blocks[min(8, max(0, int((v - lo) / span * 8)))] for v in picks)


class _BigNumbers(Static):
    """Left pane: HR / SpO2 readouts."""

    hr: reactive[int] = reactive(0)
    spo2: reactive[int] = reactive(0)
    t_ms: reactive[int] = reactive(0)
    finger: reactive[bool] = reactive(False)

    def render(self) -> RenderableType:
        hr_text = Text(f"{self.hr:>3}", style="bold red") if self.hr else Text(" — ", style="dim")
        spo2_text = (
            Text(f"{self.spo2:>3}", style="bold cyan") if self.spo2 else Text(" — ", style="dim")
        )
        finger_indicator = (
            Text("● finger detected", style="bold green")
            if self.finger
            else Text("○ no finger", style="dim")
        )
        table = Table.grid(padding=(0, 2))
        table.add_column(justify="right")
        table.add_column(justify="left")
        table.add_row(Text("HR", style="bold"), hr_text + Text(" bpm"))
        table.add_row(Text("SpO₂", style="bold"), spo2_text + Text(" %"))
        table.add_row(Text("t", style="dim"), Text(f"{self.t_ms} ms", style="dim"))
        body = Group(table, Text(""), finger_indicator)
        return Panel(Align.center(body, vertical="middle"), title="vitals", border_style="cyan")


class _Sparkline(Static):
    """Right pane: HR sparkline over the last 30 s."""

    def __init__(self) -> None:
        super().__init__()
        self._history: deque[tuple[float, int]] = deque()  # (t_seconds, hr)

    def push(self, t_ms: int, hr: int) -> None:
        if hr <= 0:
            return
        t_s = t_ms / 1000.0
        self._history.append((t_s, hr))
        cutoff = t_s - _HR_HISTORY_SECONDS
        while self._history and self._history[0][0] < cutoff:
            self._history.popleft()
        self.refresh()

    def render(self) -> RenderableType:
        values = [hr for _, hr in self._history]
        spark = _sparkline(values, width=40)
        if values:
            stats = f"min {min(values)}  max {max(values)}  n {len(values)}"
        else:
            stats = "no data yet"
        body = Group(
            Align.center(Text(spark, style="bold magenta")),
            Text(""),
            Align.center(Text(stats, style="dim")),
        )
        return Panel(body, title="HR — last 30 s", border_style="magenta")


class _StatusBar(Static):
    """Bottom status: connection state, mode, decoder errors."""

    mode: reactive[str] = reactive("jsonl")
    connected: reactive[bool] = reactive(False)
    bin_errors: reactive[int] = reactive(0)
    json_dropped: reactive[int] = reactive(0)
    last_message: reactive[str] = reactive("")

    def render(self) -> RenderableType:
        conn = Text("● connected", style="green") if self.connected else Text("○ offline", style="red")
        mode = Text(f"mode={self.mode}", style="bold yellow")
        errors = Text(f"bin_err={self.bin_errors} json_drop={self.json_dropped}", style="dim")
        msg = Text(self.last_message or "", style="cyan")
        return Panel(Group(Text("  ").join([conn, mode, errors, msg])), border_style="dim")


class OxinodeTui(App[None]):
    """Top-level Textual app."""

    CSS: ClassVar[str] = """
    Screen { layout: vertical; }
    #main { height: 1fr; }
    _BigNumbers { width: 1fr; }
    _Sparkline { width: 1fr; }
    _StatusBar { height: 5; }
    """

    BINDINGS: ClassVar[list[Binding | tuple[str, str] | tuple[str, str, str]]] = [
        Binding("q", "quit", "quit"),
        Binding("b", "switch_bin", "binary mode"),
        Binding("j", "switch_jsonl", "json mode"),
        Binding("r", "reset_dsp", "reset DSP"),
    ]

    def __init__(self, link: Link) -> None:
        super().__init__()
        self._link: Link = link
        self._numbers: _BigNumbers = _BigNumbers()
        self._spark: _Sparkline = _Sparkline()
        self._status: _StatusBar = _StatusBar()
        self._last_status_finger: bool | None = None

    def compose(self) -> ComposeResult:
        yield Header()
        yield Horizontal(self._numbers, self._spark, id="main")
        yield self._status
        yield Footer()

    def on_mount(self) -> None:
        self._status.connected = self._link._t.is_open  # noqa: SLF001 — internal status read
        self._status.mode = self._link.mode.value
        self.set_interval(_POLL_INTERVAL, self._tick)

    # ── Actions ─────────────────────────────────────────────────────────

    def action_switch_bin(self) -> None:
        ok = self._link.switch_to_binary()
        self._status.mode = self._link.mode.value
        self._status.last_message = "binary mode" if ok else "BIN switch timed out"

    def action_switch_jsonl(self) -> None:
        ok = self._link.switch_to_jsonl()
        self._status.mode = self._link.mode.value
        self._status.last_message = "JSON-Lines mode" if ok else "JSONL switch timed out"

    def action_reset_dsp(self) -> None:
        if self._link.mode is not LinkMode.BIN:
            self._status.last_message = "reset DSP requires binary mode"
            return
        self._link.reset_dsp()
        self._status.last_message = "RESET_DSP sent"

    # ── Internal poll ───────────────────────────────────────────────────

    def _tick(self) -> None:
        try:
            events = self._link.poll(max_bytes=4096, timeout=0.0)
        except Exception as exc:  # noqa: BLE001 — surface any failure to UI
            self._status.connected = False
            self._status.last_message = f"link error: {exc}"
            return
        for ev in events:
            self._handle_event(ev)
        # Reflect decoder counters.
        self._status.bin_errors = self._link._bin.error_count  # noqa: SLF001
        self._status.json_dropped = self._link._json.dropped_count  # noqa: SLF001

    def _handle_event(self, ev: object) -> None:
        if isinstance(ev, Sample):
            self._numbers.hr = ev.hr
            self._numbers.spo2 = ev.spo2
            self._numbers.t_ms = ev.t_ms
            if self._last_status_finger is None:
                self._numbers.finger = ev.ir > _IR_FINGER_THRESHOLD
            self._spark.push(ev.t_ms, ev.hr)
        elif isinstance(ev, StatusEvent):
            self._last_status_finger = bool(ev.state & 0x01)
            self._numbers.finger = self._last_status_finger
            self._status.last_message = f"status state=0x{ev.state:02x} t={ev.temp_celsius:.1f}°C"
        elif isinstance(ev, AckEvent):
            self._status.last_message = f"ACK result=0x{ev.result:02x}"
        elif isinstance(ev, BannerEvent):
            self._status.last_message = f"banner {ev.schema} mode={ev.mode}"
            self._status.mode = self._link.mode.value
