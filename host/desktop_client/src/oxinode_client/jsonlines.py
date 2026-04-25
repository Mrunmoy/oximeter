"""Line-buffered JSON-Lines reader for the OxiNode USB-CDC link.

The device sends one JSON object per line, ``\\n``-terminated. On boot it
emits a schema banner of the form ``{"oxinode":"v1","mode":"jsonl"}``; the
reader records this and exposes it via :attr:`JsonLineReader.mode`.

Malformed lines are dropped with a logged warning; partial reads (anything
not ending in ``\\n``) are buffered and concatenated with subsequent feeds.
"""

from __future__ import annotations

import json
import logging
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from typing import Any, Final

__all__ = ["JsonLineReader", "ParsedLine"]

_LOG: Final[logging.Logger] = logging.getLogger(__name__)


@dataclass(frozen=True)
class ParsedLine:
    """One successfully parsed JSON object plus the prevailing mode."""

    data: dict[str, Any]
    mode: str


class JsonLineReader:
    """Streaming, partial-read tolerant JSON-Lines reader.

    Default ``mode`` is ``"jsonl"`` until a banner line of the form
    ``{"oxinode":"vX","mode":"<word>"}`` is seen, at which point ``mode`` is
    updated and the banner is *also* yielded as a regular :class:`ParsedLine`
    so downstream code can react.
    """

    def __init__(self) -> None:
        self._buf: bytearray = bytearray()
        self._mode: str = "jsonl"
        self._dropped: int = 0

    @property
    def mode(self) -> str:
        return self._mode

    @property
    def dropped_count(self) -> int:
        """Number of malformed lines dropped so far."""
        return self._dropped

    def feed(self, data: bytes | bytearray | memoryview) -> list[ParsedLine]:
        """Append bytes to the internal buffer; return any complete lines parsed."""
        if not data:
            return []
        self._buf.extend(data)
        return list(self._drain())

    def __iter__(self) -> Iterator[ParsedLine]:
        return iter(self._drain())

    def _drain(self) -> Iterable[ParsedLine]:
        while True:
            try:
                idx = self._buf.index(0x0A)  # '\n'
            except ValueError:
                return
            raw = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            line = raw.rstrip(b"\r")
            if not line:
                continue
            parsed = self._parse(line)
            if parsed is not None:
                yield parsed

    def _parse(self, line: bytes) -> ParsedLine | None:
        try:
            text = line.decode("utf-8")
        except UnicodeDecodeError:
            self._dropped += 1
            _LOG.warning("dropping non-UTF-8 JSON line: %r", line[:64])
            return None
        try:
            obj = json.loads(text)
        except json.JSONDecodeError as exc:
            self._dropped += 1
            _LOG.warning("dropping malformed JSON line: %s; raw=%r", exc, text[:120])
            return None
        if not isinstance(obj, dict):
            self._dropped += 1
            _LOG.warning("dropping non-object JSON line: %r", text[:120])
            return None
        # Schema banner detection.
        if "oxinode" in obj and isinstance(obj.get("mode"), str):
            self._mode = obj["mode"]
        return ParsedLine(data=obj, mode=self._mode)
