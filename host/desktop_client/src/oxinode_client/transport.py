"""Serial transport for the OxiNode USB-CDC link.

Opens a 115200 8N1 connection to ``/dev/ttyACM*`` (or anything matching the
RP2040 / ESP32-S3 USB VID/PID), exposes blocking ``read_bytes`` /
``write_bytes`` helpers, and detects unplug events by inspecting
``serial.SerialException`` raised from the underlying ``pyserial`` calls.
"""

from __future__ import annotations

import glob
import logging
from dataclasses import dataclass
from types import TracebackType
from typing import Final

import serial
from serial.tools import list_ports

__all__ = ["SerialTransport", "TransportError", "PortInfo", "list_candidate_ports"]

_LOG: Final[logging.Logger] = logging.getLogger(__name__)

# RP2040 (Raspberry Pi) and ESP32-S3 (Espressif) USB vendor IDs.
_KNOWN_VIDS: Final[frozenset[int]] = frozenset({0x2E8A, 0x303A})


class TransportError(RuntimeError):
    """Raised on serial open / read / write failures (incl. unplug)."""


@dataclass(frozen=True)
class PortInfo:
    """One auto-detection candidate."""

    device: str
    description: str
    vid: int | None
    pid: int | None
    serial_number: str | None


def list_candidate_ports() -> list[PortInfo]:
    """Enumerate serial ports likely to be an OxiNode device.

    Order: USB-VID match first, then anything else that looks like a CDC ACM
    device. The result always includes raw ``/dev/ttyACM*`` paths even if
    pyserial's enumerator missed them (some kernels don't surface VID/PID).
    """
    seen: set[str] = set()
    out: list[PortInfo] = []
    for p in list_ports.comports():
        if p.vid in _KNOWN_VIDS:
            out.append(
                PortInfo(
                    device=p.device,
                    description=p.description or "",
                    vid=p.vid,
                    pid=p.pid,
                    serial_number=p.serial_number,
                )
            )
            seen.add(p.device)
    for p in list_ports.comports():
        if p.device in seen:
            continue
        if p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB"):
            out.append(
                PortInfo(
                    device=p.device,
                    description=p.description or "",
                    vid=p.vid,
                    pid=p.pid,
                    serial_number=p.serial_number,
                )
            )
            seen.add(p.device)
    for dev in sorted(glob.glob("/dev/ttyACM*")):
        if dev in seen:
            continue
        out.append(PortInfo(device=dev, description="", vid=None, pid=None, serial_number=None))
        seen.add(dev)
    return out


def _autodetect() -> str:
    candidates = list_candidate_ports()
    if not candidates:
        raise TransportError(
            "no candidate serial port found (looked for VID 2e8a/303a or /dev/ttyACM*)"
        )
    return candidates[0].device


class SerialTransport:
    """Blocking serial transport with unplug detection.

    ``port=None`` triggers auto-detection. On any I/O failure the underlying
    handle is closed and a :class:`TransportError` is raised.
    """

    def __init__(
        self,
        port: str | None = None,
        baudrate: int = 115200,
        timeout: float = 0.1,
        write_timeout: float = 1.0,
    ) -> None:
        self._port: str = port if port is not None else _autodetect()
        self._baud: int = baudrate
        self._timeout: float = timeout
        self._write_timeout: float = write_timeout
        self._ser: serial.Serial | None = None

    @property
    def port(self) -> str:
        return self._port

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def open(self) -> None:
        if self.is_open:
            return
        try:
            self._ser = serial.Serial(
                port=self._port,
                baudrate=self._baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self._timeout,
                write_timeout=self._write_timeout,
                rtscts=False,
                dsrdtr=False,
                xonxoff=False,
            )
        except (OSError, serial.SerialException) as exc:
            raise TransportError(f"failed to open {self._port}: {exc}") from exc
        _LOG.info("opened %s @ %d 8N1", self._port, self._baud)

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except (OSError, serial.SerialException):
                pass
            self._ser = None

    def __enter__(self) -> SerialTransport:
        self.open()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def read_bytes(self, n: int, timeout: float | None = None) -> bytes:
        """Read up to ``n`` bytes. Honour an optional one-shot timeout override."""
        if not self.is_open or self._ser is None:
            raise TransportError("transport not open")
        prev: float | None = None
        if timeout is not None:
            prev = self._ser.timeout
            self._ser.timeout = timeout
        try:
            data = self._ser.read(n)
        except (OSError, serial.SerialException) as exc:
            self.close()
            raise TransportError(f"read failed (device unplugged?): {exc}") from exc
        finally:
            if prev is not None and self._ser is not None:
                self._ser.timeout = prev
        return bytes(data)

    def read_available(self) -> bytes:
        """Read whatever is currently in the OS buffer plus one timeout slot."""
        if not self.is_open or self._ser is None:
            raise TransportError("transport not open")
        try:
            waiting = self._ser.in_waiting
            if waiting:
                return bytes(self._ser.read(waiting))
            return bytes(self._ser.read(1))
        except (OSError, serial.SerialException) as exc:
            self.close()
            raise TransportError(f"read failed (device unplugged?): {exc}") from exc

    def write_bytes(self, data: bytes) -> int:
        if not self.is_open or self._ser is None:
            raise TransportError("transport not open")
        try:
            written = self._ser.write(data)
            self._ser.flush()
        except (OSError, serial.SerialException) as exc:
            self.close()
            raise TransportError(f"write failed (device unplugged?): {exc}") from exc
        return int(written or 0)
