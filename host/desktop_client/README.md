# oxinode-client

Python desktop client for the **OxiNode** RP2040 USB-CDC link. Reads either
the JSON-Lines wire mode (default) or the length-prefixed CRC-16/CCITT-FALSE
binary mode, renders a Textual TUI with live HR / SpO2 numbers and a 30-second
HR sparkline, and can capture samples to CSV.

The wire protocol is fully specified in `docs/PROTOCOL.md` at the repo root;
this client implements it bit-for-bit.

## Requirements

- **Python 3.10 or newer** (uses `match` statements and PEP 604 unions).
- Linux, macOS, or Windows with a working `pyserial`. On Linux you usually
  need to be in the `dialout` group to read `/dev/ttyACM*` without `sudo`.

## Install

From a clone of the OxiNode repo:

```bash
cd host/desktop_client
python -m venv .venv && source .venv/bin/activate
pip install -e '.[dev]'
```

Or, when you want a standalone install on a workstation that is not the
development one:

```bash
pipx install .
```

Inside the project's Nix dev shell (`nix develop` at the repo root), the
required Python interpreter is already on PATH; run `pip install -e .` from
this directory and you're set.

## Run

```bash
oxinode info                    # list candidate ports
oxinode monitor                 # auto-detect port, JSON mode, full TUI
oxinode monitor --mode bin      # start in binary mode
oxinode monitor --no-tui        # plain stdout, useful in scripts
oxinode capture -o samples.csv  # record to CSV until ^C
```

The TUI key bindings:

| Key | Action                                         |
|-----|------------------------------------------------|
| `b` | Switch to binary wire mode                     |
| `j` | Switch back to JSON-Lines wire mode            |
| `r` | Send `CTRL RESET_DSP` (binary mode only)       |
| `q` | Quit                                            |

A screen recording / asciicast goes here once the firmware is wired up:

```
[ docs/img/oxinode-tui.gif – placeholder ]
```

## Troubleshooting

**`Permission denied: '/dev/ttyACM0'`** — add yourself to `dialout` and
re-login:

```bash
sudo usermod -aG dialout "$USER"
```

**No port detected by `oxinode info`** — make sure the device is *not* in
BOOTSEL mode (in BOOTSEL it appears as a mass-storage device with VID
`2e8a:0003`, not a serial port). `lsusb | grep -i 2e8a` should show
`2e8a:000a` or similar once the OxiNode firmware is running.

**Garbled output in JSON mode** — most often a baud-rate mismatch with a
stale `picocom` instance. Close any other terminal program holding the port.

**TUI looks wrong** — set `TERM=xterm-256color` (or any 256-colour terminal),
and ensure the terminal is at least 80x24.

## Development

```bash
python -m pytest -q                       # tests
python -m mypy --strict src/oxinode_client # types
python -m ruff check src tests             # lint
```
