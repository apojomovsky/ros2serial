#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Turn one session vector into the wire frames the PIC sim run scripts.

A session vector carries decoded frames (topic id, sequence, payload, CRC).
The simulator needs wire bytes, so every frame goes through the reference
rCOBS encoder and gains its 0x00 delimiter, which is the same reference the
C core and the bridge are checked against.

Writes into the output directory:

- ``rx.txt`` the host frames, one wire frame per line, in the transcript's
  order, which is what the stimulus script feeds the UART.
- ``want_mcu.txt`` every MCU frame, one wire frame per line, in order, which
  is what the firmware must send back.

Exits 1 with a message naming the step when a frame will not encode.
"""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen.reference import rcobs
from r2s_gen.reference.errors import Rejected


def wire(frame: bytes) -> str:
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

    host: list[str] = []
    mcu: list[str] = []
    for index, step in enumerate(steps):
        raw = bytes.fromhex(step['frame'].replace(' ', ''))
        try:
            encoded = wire(raw)
        except Rejected as exc:
            print(
                f'pic-sim-frames: step {index} ({step["from"]}) will not encode: {exc}',
                file=sys.stderr,
            )
            return 1
        (host if step['from'] == 'host' else mcu).append(encoded)

    if not host or not mcu:
        print(
            f'pic-sim-frames: {vector} carries no {"host" if not host else "MCU"} frame',
            file=sys.stderr,
        )
        return 1
    out.mkdir(parents=True, exist_ok=True)
    (out / 'rx.txt').write_text('\n'.join(host) + '\n')
    (out / 'want_mcu.txt').write_text('\n'.join(mcu) + '\n')
    print(f'pic-sim-frames: {len(host)} host frames in, {len(mcu)} MCU frames expected')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
