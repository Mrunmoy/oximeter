"""OxiNode desktop client package.

Public surface re-exports the wire-protocol primitives so callers can write
``from oxinode_client import SampleFrame, FrameDecoder`` without reaching into
sub-modules.
"""

from __future__ import annotations

from .binframe import (
    STX,
    AckFrame,
    AckResult,
    CtrlCmd,
    CtrlFrame,
    DecodedFrame,
    FrameDecoder,
    FrameType,
    SampleFrame,
    StatusFrame,
    build_frame,
    crc16_ccitt_false,
    decode_ack,
    decode_ctrl,
    decode_sample,
    decode_status,
    encode_ack,
    encode_ctrl,
    encode_sample,
    encode_status,
)
from .jsonlines import JsonLineReader, ParsedLine

__version__: str = "0.1.0"

__all__ = [
    "__version__",
    "STX",
    "AckFrame",
    "AckResult",
    "CtrlCmd",
    "CtrlFrame",
    "DecodedFrame",
    "FrameDecoder",
    "FrameType",
    "SampleFrame",
    "StatusFrame",
    "build_frame",
    "crc16_ccitt_false",
    "decode_ack",
    "decode_ctrl",
    "decode_sample",
    "decode_status",
    "encode_ack",
    "encode_ctrl",
    "encode_sample",
    "encode_status",
    "JsonLineReader",
    "ParsedLine",
]
