# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The hardware check against an emulated Uno, so the tool is proven before a board.

Nothing but the upload needs real hardware, so the check's own path runs here:
it builds the sketch, launches the real bridge on the harness pty, samples
/button, judges the counters and asks about the LED. --skip-flash stands in
for arduino-cli, which has no simulator. The second case flashes the blink
example, which publishes no /button, and must fail with a reason naming it
instead of reporting a pass. tests/e2e/run-avr.sh runs this file beside the
scenarios, where the harness, the bridge install and the AVR core exist.
"""

import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests' / 'e2e'))

import hardware_check  # noqa: E402
from e2e_support import avr  # noqa: E402

BOARD = 'uno'
# Paced simulated time holds the sketch's 100 ms publish interval to the wall
# clock, so a ten second window measures the rate a board would show.
SECONDS = '10'
CHECK_TIMEOUT = 240.0
PTY_TIMEOUT = 20.0
RATE = re.compile(r'^button rate: ([0-9.]+) Hz from ([0-9]+) messages$', re.MULTILINE)


@pytest.fixture(scope='module')
def bridge_install():
    """The bridge workspace the check launches, built once for both cases."""
    install = hardware_check.build_dir() / 'bridge' / 'install'
    if not (install / 'setup.bash').is_file():
        subprocess.run(['bash', 'scripts/bridge-test.sh', '--build-only'], cwd=ROOT, check=True)
    assert (install / 'setup.bash').is_file(), 'the bridge build left no install'
    return install


def run_check(port, answer, sketch=hardware_check.SKETCH):
    """Run the check against one port, answering the LED question."""
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / 'tools' / 'hardware_check.py'),
            '--board',
            BOARD,
            '--port',
            port,
            '--seconds',
            SECONDS,
            '--sketch',
            sketch,
            '--skip-flash',
            '--skip-bridge-build',
        ],
        cwd=ROOT,
        input=answer + '\n',
        capture_output=True,
        text=True,
        timeout=CHECK_TIMEOUT,
    )


def spawn(elf):
    """Start one ELF paced under the harness, returning the process and its pty."""
    proc = subprocess.Popen(
        [
            str(avr.harness_binary()),
            '--mcu',
            avr.board_mcu(BOARD),
            '--firmware',
            str(elf),
            '--run-ms',
            '300000',
            '--pace',
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    # libsimavr's loader prints its own progress on stdout, so the pty line is
    # read past whatever precedes it.
    deadline = time.monotonic() + PTY_TIMEOUT
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        if line.startswith(b'r2s_simavr: pty='):
            return proc, avr.parse_pty_line(line.decode())
    proc.kill()
    proc.wait(timeout=10.0)
    raise AssertionError(f'the harness announced no pty within {PTY_TIMEOUT:.0f} s')


def stop(proc):
    """Terminate the harness, killing it when it ignores the signal."""
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.communicate(timeout=10.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()


def test_the_check_passes_on_the_emulated_uno(bridge_install):
    elf = hardware_check.build_firmware(BOARD, hardware_check.SKETCH, hardware_check.build_dir())
    proc, pty = spawn(elf)
    try:
        result = run_check(pty, 'y')
    finally:
        stop(proc)
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert lines[0] == '== ros2serial hardware check'
    assert lines[-1] == 'verdict: PASS', result.stdout
    for field in ('board: uno', 'flash: ', 'handshake: ', 'crc errors: 0', 'led: lit'):
        assert field in result.stdout, (field, result.stdout)
    measured = RATE.search(result.stdout)
    assert measured is not None, result.stdout
    assert 8.0 <= float(measured.group(1)) <= 12.0, result.stdout


def test_a_sketch_without_the_publisher_fails_with_a_readable_verdict(bridge_install):
    sketch = 'examples/arduino_blink'
    elf = hardware_check.build_firmware(BOARD, sketch, hardware_check.build_dir())
    proc, pty = spawn(elf)
    try:
        result = run_check(pty, 'n', sketch=sketch)
    finally:
        stop(proc)
    assert result.returncode == 1, result.stdout + result.stderr
    assert result.stdout.splitlines()[-1] == 'verdict: FAIL', result.stdout
    assert 'failed: the sample holds 0 /button messages' in result.stdout, result.stdout
    assert 'button rate: n/a' in result.stdout
