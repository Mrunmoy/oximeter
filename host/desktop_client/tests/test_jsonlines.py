"""Unit tests for :mod:`oxinode_client.jsonlines`."""

from __future__ import annotations

import logging

from oxinode_client.jsonlines import JsonLineReader


def test_partial_line_is_buffered() -> None:
    reader = JsonLineReader()
    assert reader.feed(b'{"t":1,"ir":2,"red":3,"hr":') == []
    out = reader.feed(b'70,"spo2":98}\n')
    assert len(out) == 1
    assert out[0].data == {"t": 1, "ir": 2, "red": 3, "hr": 70, "spo2": 98}
    assert out[0].mode == "jsonl"


def test_multiple_lines_in_one_chunk() -> None:
    reader = JsonLineReader()
    chunk = b'{"a":1}\n{"a":2}\n{"a":3}\n'
    out = reader.feed(chunk)
    assert [p.data["a"] for p in out] == [1, 2, 3]


def test_malformed_json_is_skipped(caplog: logging.LogCaptureFixture) -> None:
    reader = JsonLineReader()
    with caplog.at_level(logging.WARNING):
        out = reader.feed(b'{"good":1}\nnot-json\n{"good":2}\n')
    assert [p.data["good"] for p in out] == [1, 2]
    assert reader.dropped_count == 1
    assert any("malformed" in rec.message.lower() for rec in caplog.records)


def test_non_object_json_is_skipped() -> None:
    reader = JsonLineReader()
    out = reader.feed(b"42\n[1,2,3]\n")
    assert out == []
    assert reader.dropped_count == 2


def test_mode_banner_is_detected_and_yielded() -> None:
    reader = JsonLineReader()
    out = reader.feed(b'{"oxinode":"v1","mode":"jsonl"}\n{"t":1,"ir":2,"red":3,"hr":4,"spo2":5}\n')
    assert len(out) == 2
    assert out[0].data == {"oxinode": "v1", "mode": "jsonl"}
    assert out[0].mode == "jsonl"
    assert reader.mode == "jsonl"
    # Now switch to bin via banner.
    out = reader.feed(b'{"oxinode":"v1","mode":"bin"}\n')
    assert len(out) == 1
    assert reader.mode == "bin"


def test_crlf_is_tolerated() -> None:
    reader = JsonLineReader()
    out = reader.feed(b'{"a":1}\r\n{"a":2}\r\n')
    assert [p.data["a"] for p in out] == [1, 2]


def test_empty_lines_are_ignored() -> None:
    reader = JsonLineReader()
    out = reader.feed(b'\n\n{"a":1}\n\n')
    assert len(out) == 1
    assert out[0].data["a"] == 1


def test_non_utf8_line_is_skipped(caplog: logging.LogCaptureFixture) -> None:
    reader = JsonLineReader()
    with caplog.at_level(logging.WARNING):
        out = reader.feed(b"\xff\xfe\xfd\n")
    assert out == []
    assert reader.dropped_count == 1
