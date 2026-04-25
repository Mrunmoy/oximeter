"""Binary wire-protocol framing for the OxiNode RP2040 USB-CDC link.

Frame layout (all multi-byte fields are little-endian):

    [STX=0xAA][LEN:u16][TYPE:u8][PAYLOAD ...][CRC16:u16]

* ``LEN`` counts ``TYPE + PAYLOAD`` (i.e. excludes STX/LEN/CRC).
* ``CRC`` is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no
  xorout) computed over ``LEN + TYPE + PAYLOAD``.

The :class:`FrameDecoder` is a streaming state machine that consumes bytes
incrementally and yields ``(type, payload)`` tuples. On a CRC mismatch or any
other corruption it hunts forward for the next STX byte and resumes; this
matches the firmware's resync expectation described in ``docs/PROTOCOL.md``.
"""

from __future__ import annotations

import struct
from collections import deque
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from enum import IntEnum
from typing import Final

__all__ = [
    "STX",
    "FrameType",
    "CtrlCmd",
    "AckResult",
    "SampleFrame",
    "StatusFrame",
    "AckFrame",
    "CtrlFrame",
    "crc16_ccitt_false",
    "build_frame",
    "encode_sample",
    "decode_sample",
    "encode_status",
    "decode_status",
    "encode_ack",
    "decode_ack",
    "encode_ctrl",
    "decode_ctrl",
    "FrameDecoder",
    "DecodedFrame",
]

STX: Final[int] = 0xAA


class FrameType(IntEnum):
    """Append-only TYPE byte enum. Never repurpose a value."""

    SAMPLE = 0x01
    STATUS = 0x02
    ACK = 0x03
    CTRL = 0xFE


class CtrlCmd(IntEnum):
    """CTRL frame command codes (host -> device)."""

    STOP_BIN = 0x00
    RESET_DSP = 0x01


class AckResult(IntEnum):
    """ACK ``result`` byte. Device-side semantics."""

    OK = 0x00
    ERROR = 0x01


# ── CRC-16/CCITT-FALSE ───────────────────────────────────────────────────────

_CRC_POLY: Final[int] = 0x1021
_CRC_INIT: Final[int] = 0xFFFF
_CRC_TABLE: list[int] | None = None


def _crc_table() -> list[int]:
    """Lazily build the 256-entry lookup table."""
    global _CRC_TABLE
    if _CRC_TABLE is not None:
        return _CRC_TABLE
    table: list[int] = []
    for i in range(256):
        crc = (i << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ _CRC_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
        table.append(crc)
    _CRC_TABLE = table
    return table


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    """Compute CRC-16/CCITT-FALSE over ``data``.

    Reference vector: ``crc16_ccitt_false(b"123456789") == 0x29B1``.
    """
    crc = _CRC_INIT
    table = _crc_table()
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ table[((crc >> 8) ^ byte) & 0xFF]
    return crc


# ── Dataclasses ──────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class SampleFrame:
    """SAMPLE payload (TYPE=0x01)."""

    t_ms: int
    ir: int
    red: int
    hr: int
    spo2: int


@dataclass(frozen=True)
class StatusFrame:
    """STATUS payload (TYPE=0x02). ``temp_q8_8`` is signed Q8.8 fixed point."""

    state: int
    fifo: int
    temp_q8_8: int

    @property
    def temp_celsius(self) -> float:
        return self.temp_q8_8 / 256.0


@dataclass(frozen=True)
class AckFrame:
    """ACK payload (TYPE=0x03)."""

    result: int


@dataclass(frozen=True)
class CtrlFrame:
    """CTRL payload (TYPE=0xFE)."""

    cmd: int


# ── Encoders / decoders ──────────────────────────────────────────────────────


_SAMPLE_STRUCT: Final[struct.Struct] = struct.Struct("<IIIBB")
_STATUS_STRUCT: Final[struct.Struct] = struct.Struct("<BBh")
_ACK_STRUCT: Final[struct.Struct] = struct.Struct("<B")
_CTRL_STRUCT: Final[struct.Struct] = struct.Struct("<B")


def _check_uint(value: int, bits: int, name: str) -> None:
    if not 0 <= value < (1 << bits):
        raise ValueError(f"{name} out of range for u{bits}: {value}")


def encode_sample(s: SampleFrame) -> bytes:
    _check_uint(s.t_ms, 32, "t_ms")
    _check_uint(s.ir, 32, "ir")
    _check_uint(s.red, 32, "red")
    _check_uint(s.hr, 8, "hr")
    _check_uint(s.spo2, 8, "spo2")
    return build_frame(FrameType.SAMPLE, _SAMPLE_STRUCT.pack(s.t_ms, s.ir, s.red, s.hr, s.spo2))


def decode_sample(payload: bytes) -> SampleFrame:
    if len(payload) != _SAMPLE_STRUCT.size:
        raise ValueError(f"SAMPLE payload length {len(payload)} != {_SAMPLE_STRUCT.size}")
    t_ms, ir, red, hr, spo2 = _SAMPLE_STRUCT.unpack(payload)
    return SampleFrame(t_ms=t_ms, ir=ir, red=red, hr=hr, spo2=spo2)


def encode_status(s: StatusFrame) -> bytes:
    _check_uint(s.state, 8, "state")
    _check_uint(s.fifo, 8, "fifo")
    if not -32768 <= s.temp_q8_8 < 32768:
        raise ValueError(f"temp_q8_8 out of i16 range: {s.temp_q8_8}")
    return build_frame(FrameType.STATUS, _STATUS_STRUCT.pack(s.state, s.fifo, s.temp_q8_8))


def decode_status(payload: bytes) -> StatusFrame:
    if len(payload) != _STATUS_STRUCT.size:
        raise ValueError(f"STATUS payload length {len(payload)} != {_STATUS_STRUCT.size}")
    state, fifo, temp = _STATUS_STRUCT.unpack(payload)
    return StatusFrame(state=state, fifo=fifo, temp_q8_8=temp)


def encode_ack(a: AckFrame) -> bytes:
    _check_uint(a.result, 8, "result")
    return build_frame(FrameType.ACK, _ACK_STRUCT.pack(a.result))


def decode_ack(payload: bytes) -> AckFrame:
    if len(payload) != _ACK_STRUCT.size:
        raise ValueError(f"ACK payload length {len(payload)} != {_ACK_STRUCT.size}")
    (result,) = _ACK_STRUCT.unpack(payload)
    return AckFrame(result=result)


def encode_ctrl(c: CtrlFrame) -> bytes:
    _check_uint(c.cmd, 8, "cmd")
    return build_frame(FrameType.CTRL, _CTRL_STRUCT.pack(c.cmd))


def decode_ctrl(payload: bytes) -> CtrlFrame:
    if len(payload) != _CTRL_STRUCT.size:
        raise ValueError(f"CTRL payload length {len(payload)} != {_CTRL_STRUCT.size}")
    (cmd,) = _CTRL_STRUCT.unpack(payload)
    return CtrlFrame(cmd=cmd)


def build_frame(type_code: int, payload: bytes) -> bytes:
    """Wrap ``payload`` in an OxiNode binary frame with CRC."""
    if not 0 <= type_code <= 0xFF:
        raise ValueError(f"type_code out of range: {type_code}")
    body_len = 1 + len(payload)  # TYPE + PAYLOAD
    if body_len > 0xFFFF:
        raise ValueError(f"frame body too large: {body_len}")
    header = struct.pack("<BHB", STX, body_len, type_code)
    crc_input = header[1:] + payload  # LEN + TYPE + PAYLOAD
    crc = crc16_ccitt_false(crc_input)
    return header + payload + struct.pack("<H", crc)


# ── Streaming decoder ────────────────────────────────────────────────────────


@dataclass(frozen=True)
class DecodedFrame:
    """A successfully decoded frame yielded by :class:`FrameDecoder`."""

    type: int
    payload: bytes


class _State(IntEnum):
    HUNT = 0
    LEN_LO = 1
    LEN_HI = 2
    BODY = 3
    CRC_LO = 4
    CRC_HI = 5


class FrameDecoder:
    """Streaming, resync-tolerant decoder for OxiNode binary frames.

    Feed bytes via :meth:`feed`; iterate the returned iterable to consume
    frames. On any framing or CRC error the decoder discards the partial
    frame, retains any buffered bytes that came after the corruption, and
    resumes by hunting forward for the next STX byte.
    """

    # Maximum body length we'll honour. Real frames are far smaller; this is a
    # belt-and-braces upper bound to bound memory if we sync onto random data.
    _MAX_BODY: Final[int] = 1024

    def __init__(self) -> None:
        self._buf: deque[int] = deque()
        self._state: _State = _State.HUNT
        self._len: int = 0
        self._body: bytearray = bytearray()
        self._crc_lo: int = 0
        self._errors: int = 0

    @property
    def error_count(self) -> int:
        """Number of frames discarded due to CRC mismatch or oversize LEN."""
        return self._errors

    def feed(self, data: bytes | bytearray | memoryview) -> list[DecodedFrame]:
        """Push bytes into the decoder. Returns any frames completed."""
        self._buf.extend(data)
        return list(self._drain())

    def __iter__(self) -> Iterator[DecodedFrame]:
        return iter(self._drain())

    # ── internal ─────────────────────────────────────────────────────────

    def _drain(self) -> Iterable[DecodedFrame]:
        while self._buf:
            byte = self._buf.popleft()
            result = self._step(byte)
            if result is not None:
                yield result

    def _resync(self) -> None:
        """Reset state machine and hunt forward for the next STX."""
        self._state = _State.HUNT
        self._len = 0
        self._body.clear()
        self._crc_lo = 0

    def _step(self, byte: int) -> DecodedFrame | None:
        match self._state:
            case _State.HUNT:
                if byte == STX:
                    self._state = _State.LEN_LO
                return None
            case _State.LEN_LO:
                self._len = byte
                self._state = _State.LEN_HI
                return None
            case _State.LEN_HI:
                self._len |= byte << 8
                if self._len < 1 or self._len > self._MAX_BODY:
                    # Implausible length — treat as desync. Note: this byte
                    # might itself be an STX, so retry from HUNT and re-feed.
                    self._errors += 1
                    self._resync()
                    if byte == STX:
                        self._state = _State.LEN_LO
                    return None
                self._body.clear()
                self._state = _State.BODY
                return None
            case _State.BODY:
                self._body.append(byte)
                if len(self._body) >= self._len:
                    self._state = _State.CRC_LO
                return None
            case _State.CRC_LO:
                self._crc_lo = byte
                self._state = _State.CRC_HI
                return None
            case _State.CRC_HI:
                received_crc = self._crc_lo | (byte << 8)
                # CRC is over LEN + TYPE + PAYLOAD.
                crc_input = bytes([self._len & 0xFF, (self._len >> 8) & 0xFF]) + bytes(self._body)
                expected = crc16_ccitt_false(crc_input)
                type_code = self._body[0]
                payload = bytes(self._body[1:])
                if received_crc == expected:
                    self._resync()
                    return DecodedFrame(type=type_code, payload=payload)
                # CRC mismatch: discard frame, hunt forward.
                self._errors += 1
                self._resync()
                return None
        return None
