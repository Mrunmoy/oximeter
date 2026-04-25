"""Unit tests for :mod:`oxinode_client.binframe`."""

from __future__ import annotations

import struct

import pytest

from oxinode_client.binframe import (
    STX,
    AckFrame,
    CtrlCmd,
    CtrlFrame,
    FrameDecoder,
    FrameType,
    SampleFrame,
    StatusFrame,
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


def test_crc_known_vector() -> None:
    """The standard CCITT-FALSE check value: CRC of '123456789' == 0x29B1."""
    assert crc16_ccitt_false(b"123456789") == 0x29B1


def test_crc_empty_returns_init() -> None:
    assert crc16_ccitt_false(b"") == 0xFFFF


def test_crc_single_byte_matches_table() -> None:
    # CRC of a single 0x00 byte under CCITT-FALSE is 0xE1F0.
    assert crc16_ccitt_false(b"\x00") == 0xE1F0


def test_sample_roundtrip() -> None:
    s = SampleFrame(t_ms=0xDEADBEEF, ir=123_456, red=98_765, hr=72, spo2=98)
    encoded = encode_sample(s)
    assert encoded[0] == STX
    decoder = FrameDecoder()
    frames = decoder.feed(encoded)
    assert len(frames) == 1
    assert frames[0].type == FrameType.SAMPLE
    assert decode_sample(frames[0].payload) == s


def test_status_roundtrip() -> None:
    st = StatusFrame(state=0x03, fifo=12, temp_q8_8=-1234)
    encoded = encode_status(st)
    decoded = decode_status(encoded[4:-2])
    assert decoded == st
    # Whole-frame round-trip via decoder.
    frames = FrameDecoder().feed(encoded)
    assert len(frames) == 1
    assert frames[0].type == FrameType.STATUS
    assert decode_status(frames[0].payload) == st


def test_ack_roundtrip() -> None:
    a = AckFrame(result=0x00)
    encoded = encode_ack(a)
    frames = FrameDecoder().feed(encoded)
    assert len(frames) == 1
    assert frames[0].type == FrameType.ACK
    assert decode_ack(frames[0].payload) == a


def test_ctrl_roundtrip() -> None:
    c = CtrlFrame(cmd=CtrlCmd.RESET_DSP)
    encoded = encode_ctrl(c)
    frames = FrameDecoder().feed(encoded)
    assert len(frames) == 1
    assert frames[0].type == FrameType.CTRL
    assert decode_ctrl(frames[0].payload) == c


def test_decoder_handles_split_bytes() -> None:
    """Bytes split across multiple ``feed`` calls must still parse."""
    s = SampleFrame(t_ms=42, ir=10_000, red=9_000, hr=60, spo2=97)
    encoded = encode_sample(s)
    decoder = FrameDecoder()
    out: list[tuple[int, bytes]] = []
    for byte in encoded:
        out.extend((f.type, f.payload) for f in decoder.feed(bytes([byte])))
    assert len(out) == 1
    assert out[0][0] == FrameType.SAMPLE
    assert decode_sample(out[0][1]) == s


def test_decoder_resyncs_after_garbage() -> None:
    """Random prefix + a real frame should yield exactly one frame."""
    s = SampleFrame(t_ms=1, ir=1, red=1, hr=80, spo2=99)
    junk = b"\x00\x01\x02\xff\xff\xaa\x00\x00"  # includes a stray STX
    encoded = encode_sample(s)
    decoder = FrameDecoder()
    frames = decoder.feed(junk + encoded)
    # The stray STX may eat a couple of bytes but the second STX must succeed.
    assert any(decode_sample(f.payload) == s for f in frames if f.type == FrameType.SAMPLE)


def test_decoder_rejects_crc_mismatch() -> None:
    s = SampleFrame(t_ms=1, ir=2, red=3, hr=4, spo2=5)
    encoded = bytearray(encode_sample(s))
    # Flip a payload byte; CRC must now mismatch.
    encoded[5] ^= 0xFF
    decoder = FrameDecoder()
    frames = decoder.feed(bytes(encoded))
    assert frames == []
    assert decoder.error_count == 1


def test_decoder_resyncs_then_accepts_next_frame() -> None:
    bad_payload = SampleFrame(t_ms=10, ir=20, red=30, hr=40, spo2=50)
    good_payload = SampleFrame(t_ms=11, ir=21, red=31, hr=41, spo2=51)
    bad = bytearray(encode_sample(bad_payload))
    bad[6] ^= 0x80  # corrupt CRC
    good = encode_sample(good_payload)
    decoder = FrameDecoder()
    frames = decoder.feed(bytes(bad) + good)
    decoded = [decode_sample(f.payload) for f in frames if f.type == FrameType.SAMPLE]
    assert good_payload in decoded
    assert decoder.error_count >= 1


def test_decoder_oversize_len_is_treated_as_garbage() -> None:
    """A bogus LEN of 0xFFFF must not allocate huge buffers."""
    bogus = bytes([STX, 0xFF, 0xFF, 0x01, 0x00, 0x00])
    decoder = FrameDecoder()
    frames = decoder.feed(bogus)
    assert frames == []
    assert decoder.error_count == 1


def test_build_frame_layout_matches_spec() -> None:
    """Spot-check the on-wire bytes for a known SAMPLE."""
    s = SampleFrame(t_ms=0x01020304, ir=0x05060708, red=0x090A0B0C, hr=0x10, spo2=0x20)
    frame = encode_sample(s)
    # Header
    assert frame[0] == STX
    body_len = struct.unpack_from("<H", frame, 1)[0]
    assert body_len == 1 + 14  # TYPE + 14-byte payload
    assert frame[3] == FrameType.SAMPLE
    # CRC at the tail is over LEN+TYPE+PAYLOAD.
    crc_input = frame[1 : 3 + body_len]
    crc = struct.unpack_from("<H", frame, 3 + body_len)[0]
    assert crc == crc16_ccitt_false(crc_input)


def test_encode_validates_ranges() -> None:
    with pytest.raises(ValueError):
        encode_sample(SampleFrame(t_ms=-1, ir=0, red=0, hr=0, spo2=0))
    with pytest.raises(ValueError):
        encode_sample(SampleFrame(t_ms=0, ir=0, red=0, hr=300, spo2=0))
    with pytest.raises(ValueError):
        encode_status(StatusFrame(state=0, fifo=0, temp_q8_8=70_000))
