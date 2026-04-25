"""``oxinode`` CLI entry point.

Sub-commands:

* ``info`` — list candidate serial ports.
* ``monitor`` — open the TUI (or print plain lines with ``--no-tui``).
* ``capture`` — record samples to a CSV file until ^C.
"""

from __future__ import annotations

import csv
import logging
import sys
from pathlib import Path

import click
from rich.console import Console
from rich.table import Table

from . import __version__
from .link import AckEvent, BannerEvent, Link, LinkMode, Sample, StatusEvent
from .transport import SerialTransport, TransportError, list_candidate_ports
from .tui import OxinodeTui

__all__ = ["cli"]

_console: Console = Console()


def _configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        stream=sys.stderr,
    )


@click.group(help="OxiNode desktop client (RP2040 USB-CDC).")
@click.version_option(version=__version__, prog_name="oxinode")
@click.option("-v", "--verbose", is_flag=True, help="Enable debug logging.")
@click.pass_context
def cli(ctx: click.Context, verbose: bool) -> None:
    ctx.ensure_object(dict)
    ctx.obj["verbose"] = verbose
    _configure_logging(verbose)


@cli.command("info", help="List candidate serial ports for OxiNode devices.")
def info() -> None:
    ports = list_candidate_ports()
    if not ports:
        _console.print("[yellow]no candidate serial ports detected[/yellow]")
        _console.print(
            "looked for USB VID 2e8a (RP2040), 303a (Espressif), and /dev/ttyACM*"
        )
        return
    table = Table(title="OxiNode candidate ports")
    table.add_column("device")
    table.add_column("vid:pid")
    table.add_column("serial")
    table.add_column("description")
    for p in ports:
        vidpid = f"{p.vid:04x}:{p.pid:04x}" if p.vid and p.pid else "—"
        table.add_row(p.device, vidpid, p.serial_number or "—", p.description or "—")
    _console.print(table)


def _open_link(port: str | None, mode: str) -> tuple[SerialTransport, Link]:
    transport = SerialTransport(port=port)
    transport.open()
    link = Link(transport)
    if mode == "bin":
        if not link.switch_to_binary():
            _console.print("[yellow]warning: binary-mode ack not received[/yellow]")
    return transport, link


@cli.command("monitor", help="Open the live TUI (or stream plain text with --no-tui).")
@click.option("--port", "port", default=None, help="Serial port (auto-detect if omitted).")
@click.option(
    "--mode",
    "mode",
    type=click.Choice(["jsonl", "bin"]),
    default="jsonl",
    show_default=True,
    help="Initial wire mode.",
)
@click.option("--no-tui", is_flag=True, help="Print parsed events instead of running the TUI.")
def monitor(port: str | None, mode: str, no_tui: bool) -> None:
    try:
        transport, link = _open_link(port, mode)
    except TransportError as exc:
        _console.print(f"[red]{exc}[/red]")
        sys.exit(2)
    try:
        if no_tui:
            _stream_plain(link)
        else:
            app = OxinodeTui(link)
            app.run()
    finally:
        transport.close()


def _stream_plain(link: Link) -> None:
    _console.print(f"[dim]streaming in {link.mode.value} mode — ^C to quit[/dim]")
    try:
        while True:
            for ev in link.poll(max_bytes=4096, timeout=0.1):
                if isinstance(ev, Sample):
                    _console.print(
                        f"t={ev.t_ms} ir={ev.ir} red={ev.red} hr={ev.hr} spo2={ev.spo2}"
                    )
                elif isinstance(ev, StatusEvent):
                    _console.print(
                        f"[cyan]status[/cyan] state=0x{ev.state:02x} fifo={ev.fifo} "
                        f"temp={ev.temp_celsius:.2f}°C"
                    )
                elif isinstance(ev, AckEvent):
                    _console.print(f"[yellow]ack[/yellow] result=0x{ev.result:02x}")
                elif isinstance(ev, BannerEvent):
                    _console.print(
                        f"[magenta]banner[/magenta] schema={ev.schema} mode={ev.mode}"
                    )
    except KeyboardInterrupt:
        _console.print("[dim]bye[/dim]")


@cli.command("capture", help="Record samples to a CSV file until ^C.")
@click.option("--port", "port", default=None, help="Serial port (auto-detect if omitted).")
@click.option(
    "--mode",
    "mode",
    type=click.Choice(["jsonl", "bin"]),
    default="jsonl",
    show_default=True,
)
@click.option(
    "--output",
    "-o",
    "output",
    type=click.Path(dir_okay=False, writable=True, path_type=Path),
    required=True,
    help="Path to the output CSV file.",
)
def capture(port: str | None, mode: str, output: Path) -> None:
    try:
        transport, link = _open_link(port, mode)
    except TransportError as exc:
        _console.print(f"[red]{exc}[/red]")
        sys.exit(2)
    n_samples = 0
    try:
        with output.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["t_ms", "ir", "red", "hr", "spo2", "mode"])
            _console.print(f"[dim]capturing to {output} — ^C to stop[/dim]")
            try:
                while True:
                    for ev in link.poll(max_bytes=4096, timeout=0.1):
                        if isinstance(ev, Sample):
                            writer.writerow(
                                [ev.t_ms, ev.ir, ev.red, ev.hr, ev.spo2, link.mode.value]
                            )
                            n_samples += 1
                            if n_samples % 50 == 0:
                                fh.flush()
            except KeyboardInterrupt:
                _console.print(f"[green]captured {n_samples} samples[/green]")
    finally:
        if link.mode is LinkMode.BIN:
            link.switch_to_jsonl()
        transport.close()


if __name__ == "__main__":  # pragma: no cover
    cli()
