#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Turn one session vector into the wire frames the PIC sim run scripts.

A session vector carries decoded frames (topic id, sequence, payload, CRC).
The simulator needs wire bytes, so every frame goes through the reference
rCOBS encoder and gains its 0x00 delimiter, which is the same reference the
C core and the bridge are checked against.

Writes into the output directory:

- ``rx.txt`` the host frames, one per line: the time the transcript sends
  it, then its wire bytes as decimals. The stimulus script waits for that
  time before handing the bytes over, so the MCU's own timers see the same
  spacing the transcript recorded.
- ``want_mcu.txt`` every MCU frame, one wire frame per line as hex, in
  order, which is what the firmware must send back.

Exits 1 with a message naming the step when a frame will not encode.
"""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen.reference import rcobs  # noqa: E402


def wire(frame: bytes) -> str:
    """Encode one decoded frame as the decimal byte list MPLAB SIM reads.

    The simulator's ``packetin`` parses the rest of the line as decimal
    integers, so this prints one number per byte rather than hex.

    Args:
        frame: the decoded frame, topic id through CRC.

    Returns:
        The space-separated decimal values of the rCOBS encoding plus its
        delimiter.
    """
    return ' '.join(str(byte) for byte in rcobs.encode(frame) + b'\x00')


def wire_hex(frame: bytes) -> str:
    """Encode one decoded frame as the hex of its wire form.

    Args:
        frame: the decoded frame, topic id through CRC.

    Returns:
        The lowercase hex of the rCOBS encoding plus its delimiter.
    """
    return (rcobs.encode(frame) + b'\x00').hex()


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print('usage: pic-sim-frames.py <vector.yaml> <out-dir>', file=sys.stderr)
        return 2
    vector, out = Path(argv[1]), Path(argv[2])
    try:
        steps = yaml.safe_load(vector.read_text())['steps']
    except (OSError, KeyError, yaml.YAMLError) as exc:
        print(f'pic-sim-frames: cannot read {vector}: {exc}', file=sys.stderr)
        return 1

    host: list[tuple[int, str]] = []
    mcu: list[str] = []
    for step in steps:
        raw = bytes.fromhex(step['frame'].replace(' ', ''))
        encoded = wire(raw)
        if step['from'] == 'host':
            host.append((step['at_ms'], encoded))
        else:
            mcu.append(wire_hex(raw))

    if not host or not mcu:
        print(
            f'pic-sim-frames: {vector} carries no {"host" if not host else "MCU"} frame',
            file=sys.stderr,
        )
        return 1
    out.mkdir(parents=True, exist_ok=True)
    # The stimulus script is emitted straight-line: one `wait for N ms;` per
    # gap, then the frame. The simulator's SCL subset rejects time
    # arithmetic, so the gaps are computed here and written as literals.
    script = [
        'testbench for "PIC18F4550" is',
        'begin',
        '  process is',
        '    file InFile : text;',
        '    variable Status : file_open_status;',
        '    variable InLine : line;',
        '  begin',
        '    file_open(Status, InFile, "rx.txt", read_mode);',
        '    if Status != open_ok then',
        '      report "cannot open rx.txt" severity failure;',
        '    end if;',
    ]
    elapsed = 0
    for at_ms, _ in host:
        gap = at_ms - elapsed
        if gap > 0:
            script.append(f'    wait for {gap} ms;')
        elapsed = at_ms
        script.extend(['    readline(InFile, InLine);', '    packetin(InLine, RCREG, true);'])
    script += ['    wait;', '  end process;', 'end testbench;']
    (out / 'stimulus.scl').write_text('\n'.join(script) + '\n')
    (out / 'rx.txt').write_text('\n'.join(text for _, text in host) + '\n')
    (out / 'want_mcu.txt').write_text('\n'.join(mcu) + '\n')
    print(f'pic-sim-frames: {len(host)} host frames in, {len(mcu)} MCU frames expected')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
