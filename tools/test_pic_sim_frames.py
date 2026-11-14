# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The PIC sim frame extractor.

A session vector carries decoded frames; the simulator needs wire bytes,
which are the rCOBS encoding of each frame plus its 0x00 delimiter. The
host frames go to the stimulus script as decimal byte lists, because
`packetin` parses the rest of the line as decimal integers; the expected
MCU frames stay hex so a failure prints the bytes the vector froze.
"""

import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools' / 'pic-sim-frames.py'
sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen.reference import rcobs  # noqa: E402

# A host HELLO_REQ and an MCU HELLO, the smallest transcript that exercises
# both directions.
HELLO_REQ = '00 00 01 04 dd 5c'
HELLO = '00 00 02 04 00 00 00 00 00 02 03 02 e8 03 00 00 11 01 04 6e 6f 64 65 7c 48'


def run(tmp_path: Path, steps: list[dict[str, str]]) -> subprocess.CompletedProcess[str]:
    """Write one vector and run the extractor on it."""
    vector = tmp_path / 'vector.yaml'
    vector.write_text(yaml.safe_dump({'name': 'probe', 'covers': ['SYS-2'], 'steps': steps}))
    out = tmp_path / 'out'
    return subprocess.run(
        [sys.executable, str(TOOL), str(vector), str(out)],
        capture_output=True,
        text=True,
        cwd=ROOT,
    )


def wire_of(frame_hex: str) -> str:
    """The wire hex the vector's frame must produce."""
    return (rcobs.encode(bytes.fromhex(frame_hex.replace(' ', ''))) + b'\x00').hex()


def test_host_frames_become_decimal_byte_lists(tmp_path: Path) -> None:
    result = run(
        tmp_path,
        [
            {'from': 'host', 'frame': HELLO_REQ, 'at_ms': 0},
            {'from': 'mcu', 'frame': HELLO, 'at_ms': 0},
        ],
    )
    assert result.returncode == 0, result.stderr
    written = (tmp_path / 'out' / 'rx.txt').read_text().strip()
    assert written == ' '.join(str(byte) for byte in bytes.fromhex(wire_of(HELLO_REQ)))


def test_mcu_frames_stay_hex(tmp_path: Path) -> None:
    result = run(
        tmp_path,
        [
            {'from': 'host', 'frame': HELLO_REQ, 'at_ms': 0},
            {'from': 'mcu', 'frame': HELLO, 'at_ms': 0},
        ],
    )
    assert result.returncode == 0, result.stderr
    assert (tmp_path / 'out' / 'want_mcu.txt').read_text().strip() == wire_of(HELLO)


def test_a_gap_becomes_a_plain_wait_literal(tmp_path: Path) -> None:
    """The simulator's SCL rejects time arithmetic, so each gap is a literal."""
    result = run(
        tmp_path,
        [
            {'from': 'host', 'frame': HELLO_REQ, 'at_ms': 300},
            {'from': 'host', 'frame': '04 00 d8 97', 'at_ms': 350},
            {'from': 'mcu', 'frame': HELLO, 'at_ms': 0},
        ],
    )
    assert result.returncode == 0, result.stderr
    script = (tmp_path / 'out' / 'stimulus.scl').read_text()
    assert 'wait for 300 ms;' in script
    assert 'wait for 50 ms;' in script
    # No arithmetic sneaks in: every wait is a bare literal.
    waits = [line.strip() for line in script.splitlines() if 'wait for' in line]
    assert waits == ['wait for 300 ms;', 'wait for 50 ms;']


def test_each_frame_is_handed_over_once(tmp_path: Path) -> None:
    result = run(
        tmp_path,
        [
            {'from': 'host', 'frame': HELLO_REQ, 'at_ms': 0},
            {'from': 'host', 'frame': '04 00 d8 97', 'at_ms': 10},
            {'from': 'mcu', 'frame': HELLO, 'at_ms': 0},
        ],
    )
    assert result.returncode == 0, result.stderr
    script = (tmp_path / 'out' / 'stimulus.scl').read_text()
    assert script.count('packetin(') == 2
    assert script.count('readline(') == 2


def test_frames_keep_the_transcript_order(tmp_path: Path) -> None:
    second = '04 00 d8 97'
    result = run(
        tmp_path,
        [
            {'from': 'host', 'frame': HELLO_REQ, 'at_ms': 0},
            {'from': 'host', 'frame': second, 'at_ms': 10},
            {'from': 'mcu', 'frame': HELLO, 'at_ms': 0},
        ],
    )
    assert result.returncode == 0, result.stderr
    lines = (tmp_path / 'out' / 'rx.txt').read_text().split()
    first_frame = ' '.join(str(byte) for byte in bytes.fromhex(wire_of(HELLO_REQ))).split()
    assert lines[: len(first_frame)] == first_frame


def test_a_vector_without_host_frames_is_refused(tmp_path: Path) -> None:
    result = run(tmp_path, [{'from': 'mcu', 'frame': HELLO, 'at_ms': 0}])
    assert result.returncode == 1
    assert 'carries no host frame' in result.stderr


def test_a_vector_without_mcu_frames_is_refused(tmp_path: Path) -> None:
    result = run(tmp_path, [{'from': 'host', 'frame': HELLO_REQ, 'at_ms': 0}])
    assert result.returncode == 1
    assert 'carries no MCU frame' in result.stderr
