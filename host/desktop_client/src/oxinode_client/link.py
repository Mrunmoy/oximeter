"""Mode-aware link layer that yields :class:`Sample` events to consumers.

This sits between :class:`oxinode_client.transport.SerialTransport` and the
TUI / CLI. It starts in JSON-Lines mode (the device default), parses sample
lines via :class:`JsonLineReader`, and can switch to binary mode on demand by
sending ``MODE BIN\\n``, waiting for the JSON ack banner, and then routing
all subsequent bytes through :class:`FrameDecoder`. The reverse switch sends
a CTRL ``STOP_BIN`` frame and waits for the next JSON banner line.

Whichever wire mode is active, consumers receive a uniform stream of
:class:`Sample` named tuples plus optional :class:`StatusEvent` /
:class:`AckEvent` items.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Iterable
from dataclasses import dataclass
from enum import Enum
from typing import Final

from .binframe import (
    AckResult,
    CtrlCmd,
    CtrlFrame,
    FrameDecoder,
    FrameType,
    decode_ack,
    decode_sample,
    decode_status,
    encode_ctrl,
)
from .jsonlines import JsonLineReader
from .transport import SerialTransport, TransportError

__all__ = [
    "LinkMode",
    "Sample",
    "StatusEvent",
    "AckEvent",
    "BannerEvent",
    "LinkEvent",
    "Link",
]

_LOG: Final[logging.Logger] = logging.getLogger(__name__)


class LinkMode(str, Enum):
    JSONL = "jsonl"
    BIN = "bin"


@dataclass(frozen=True)
class Sample:
    """One IR/Red sample with derived HR / SpO2."""

    t_ms: int
    ir: int
    red: int
    hr: int
    spo2: int


@dataclass(frozen=True)
class StatusEvent:
    state: int
    fifo: int
    temp_celsius: float


@dataclass(frozen=True)
class AckEvent:
    result: int


@dataclass(frozen=True)
class BannerEvent:
    """The device emitted ``{"oxinode":"vN","mode":"..."}``."""

    schema: str
    mode: str


LinkEvent = Sample | StatusEvent | AckEvent | BannerEvent


class Link:
    """High-level mode-aware link.

    Doesn't own the transport open/close lifecycle — pass an already-opened
    :class:`SerialTransport` (or open inside a ``with`` block).
    """

    def __init__(self, transport: SerialTransport) -> None:
        self._t: SerialTransport = transport
        self._mode: LinkMode = LinkMode.JSONL
        self._json: JsonLineReader = JsonLineReader()
        self._bin: FrameDecoder = FrameDecoder()

    @property
    def mode(self) -> LinkMode:
        return self._mode

    # ── Mode switching ──────────────────────────────────────────────────

    def switch_to_binary(self, ack_timeout: float = 1.0) -> bool:
        """Send ``MODE BIN`` and wait for the JSON ack banner. Returns True on success."""
        if self._mode is LinkMode.BIN:
            return True
        _LOG.info("switching link to binary mode")
        self._t.write_bytes(b"MODE BIN\n")
        deadline = time.monotonic() + ack_timeout
        while time.monotonic() < deadline:
            chunk = self._t.read_bytes(256, timeout=0.1)
            if not chunk:
                continue
            for line in self._json.feed(chunk):
                if line.data.get("mode") == "bin":
                    self._mode = LinkMode.BIN
                    self._bin = FrameDecoder()
                    _LOG.info("device acked binary mode")
                    return True
        _LOG.warning("binary-mode ack not received within %.2fs", ack_timeout)
        return False

    def switch_to_jsonl(self, ack_timeout: float = 1.0) -> bool:
        """Send CTRL STOP_BIN; wait for a JSON banner re-confirming mode."""
        if self._mode is LinkMode.JSONL:
            return True
        _LOG.info("switching link to JSON-Lines mode")
        self._t.write_bytes(encode_ctrl(CtrlFrame(cmd=CtrlCmd.STOP_BIN)))
        deadline = time.monotonic() + ack_timeout
        while time.monotonic() < deadline:
            chunk = self._t.read_bytes(256, timeout=0.1)
            if not chunk:
                continue
            # Tail of binary stream may still be in flight — drain it through
            # the binary decoder until we find a 0x0A from the post-switch
            # banner, then hand the remainder to the JSON reader.
            for nl_idx, byte in enumerate(chunk):
                if byte == 0x0A:
                    # Heuristic: once we see a newline after STOP_BIN the
                    # device is back in text mode. Hand everything from the
                    # start of this chunk onward to JSON.
                    self._mode = LinkMode.JSONL
                    self._json = JsonLineReader()
                    for line in self._json.feed(chunk[: nl_idx + 1]):
                        if line.data.get("mode") == "jsonl":
                            _LOG.info("device acked JSON-Lines mode")
                    # Push remainder back through the JSON reader for the
                    # next ``poll`` call.
                    if nl_idx + 1 < len(chunk):
                        self._json.feed(chunk[nl_idx + 1 :])
                    return True
        _LOG.warning("JSON-Lines ack not received within %.2fs", ack_timeout)
        return False

    def reset_dsp(self) -> None:
        """Send a CTRL RESET_DSP frame (binary mode only)."""
        if self._mode is not LinkMode.BIN:
            raise RuntimeError("reset_dsp requires binary mode")
        self._t.write_bytes(encode_ctrl(CtrlFrame(cmd=CtrlCmd.RESET_DSP)))

    # ── Polling ─────────────────────────────────────────────────────────

    def poll(self, max_bytes: int = 4096, timeout: float = 0.1) -> list[LinkEvent]:
        """Read up to ``max_bytes`` and decode them; returns 0+ events."""
        try:
            chunk = self._t.read_bytes(max_bytes, timeout=timeout)
        except TransportError:
            raise
        if not chunk:
            return []
        return list(self._decode(chunk))

    def _decode(self, chunk: bytes) -> Iterable[LinkEvent]:
        if self._mode is LinkMode.JSONL:
            for line in self._json.feed(chunk):
                obj = line.data
                if "oxinode" in obj and isinstance(obj.get("mode"), str):
                    yield BannerEvent(schema=str(obj.get("oxinode", "")), mode=obj["mode"])
                    continue
                if "ir" in obj and "red" in obj:
                    try:
                        yield Sample(
                            t_ms=int(obj["t"]),
                            ir=int(obj["ir"]),
                            red=int(obj["red"]),
                            hr=int(obj["hr"]),
                            spo2=int(obj["spo2"]),
                        )
                    except (KeyError, TypeError, ValueError):
                        _LOG.warning("dropping JSON sample with bad fields: %r", obj)
                    continue
                if "status" in obj:
                    # Free-form status line; surface it as state=0xFF marker.
                    yield StatusEvent(state=0xFF, fifo=0, temp_celsius=0.0)
            return
        # Binary mode.
        for frame in self._bin.feed(chunk):
            match frame.type:
                case FrameType.SAMPLE:
                    s = decode_sample(frame.payload)
                    yield Sample(t_ms=s.t_ms, ir=s.ir, red=s.red, hr=s.hr, spo2=s.spo2)
                case FrameType.STATUS:
                    st = decode_status(frame.payload)
                    yield StatusEvent(state=st.state, fifo=st.fifo, temp_celsius=st.temp_celsius)
                case FrameType.ACK:
                    a = decode_ack(frame.payload)
                    yield AckEvent(result=a.result)
                    if a.result == AckResult.OK and self._mode is LinkMode.BIN:
                        # An ACK in binary mode is most often the response to
                        # STOP_BIN; the caller's switch_to_jsonl() will handle
                        # the actual mode flip. We just surface the event.
                        pass
                case _:
                    _LOG.warning("ignoring unknown frame type 0x%02x", frame.type)
