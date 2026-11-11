# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The simavr backend: the scenario's firmware on an emulated Uno or Mega.

Each scenario spawns the board's ELF under r2s_simavr and talks to the pty
the harness announces, reusing the bridge launcher and the shared checks.
The harness binary and the firmware directory come from tests/e2e/run-avr.sh,
which builds both; R2S_E2E_BOARD picks the board for a standalone run.
Every spawn paints the stack, and the stop summary's peak is kept per
firmware in the JSON file R2S_E2E_AVR_STACK_OUT names, so run-avr.sh can
report the measured worst case next to the static figure.
"""

import json
import os
import select
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
READY_TIMEOUT = 20.0
# The harness stops the firmware past this simulated budget; the soak raises
# it past its wall window, since simulated time can run hot. Short runs keep
# the five minute default.
RUN_MS = ('--run-ms', os.environ.get('R2S_SIMAVR_RUN_MS', '300000'))

_BOARDS = {
    'uno': ('atmega328p', 'arduino:avr:uno'),
    'mega': ('atmega2560', 'arduino:avr:mega'),
}


def board_mcu(board):
    """The simavr MCU name for the board, rejecting unknown boards."""
    assert board in _BOARDS, f'unknown avr board {board!r}: want one of {sorted(_BOARDS)}'
    return _BOARDS[board][0]


def board_fqbn(board):
    """The arduino-cli board name the slice sketch compiles for."""
    assert board in _BOARDS, f'unknown avr board {board!r}: want one of {sorted(_BOARDS)}'
    return _BOARDS[board][1]


def parse_pty_line(line):
    """The pty slave path from the harness announcement line."""
    assert line.startswith('r2s_simavr: pty='), 'no pty in harness line: ' + line.strip()
    return line.strip().split('=', 1)[1]


def harness_binary():
    """The r2s_simavr binary, built by tests/e2e/run-avr.sh."""
    given = os.environ.get('R2S_SIMAVR_BIN', '')
    assert given, 'R2S_SIMAVR_BIN is unset: run tests/e2e/run-avr.sh, which builds the harness'
    assert Path(given).is_file(), 'no harness at ' + given + ': run tests/e2e/run-avr.sh'
    return Path(given)


def firmware_image(board, flavor='slice', baud=None):
    """The board's ELF of one flavor, built by tests/e2e/run-avr.sh.

    The slice carries scenarios 1 to 4; the flood feeds scenario 12; the
    service example feeds scenario 15 and the action example scenario 16.
    A baud names one matrix image; the default baud keeps the legacy file
    name. Only the slice runs the baud matrix, so the example flavors take
    no baud.
    """
    given = os.environ.get('R2S_E2E_AVR_FW', '')
    assert given, 'R2S_E2E_AVR_FW is unset: run tests/e2e/run-avr.sh, which builds the firmware'
    assert flavor in ('slice', 'flood', 'service', 'action'), (
        f'unknown firmware flavor {flavor!r}: want slice, flood, service or action'
    )
    if flavor in ('service', 'action'):
        assert baud is None, f'no baud matrix for the {flavor} example firmware'
    if baud is None or str(baud) == '115200':
        name = f'{flavor}-{board_mcu(board)}.elf'
    else:
        name = f'{flavor}-{board_mcu(board)}-{baud}.elf'
    path = Path(given) / name
    assert path.is_file(), 'no firmware at ' + str(path) + ': run tests/e2e/run-avr.sh'
    return path


def parse_stop_summary(line):
    """The harness stop fields, with the simulated time scenario 2 gates on."""
    assert line.startswith('r2s_simavr: stop '), 'no stop summary in harness line: ' + line.strip()
    summary = {}
    for field in line.split()[2:]:
        key, _, value = field.partition('=')
        summary[key] = int(value)
    assert 'simulated_ms' in summary, 'no simulated_ms in harness line: ' + line.strip()
    return summary


def firmware_label(board, flavor='slice', baud=None):
    """The measured-stack key for one firmware: flavor plus MCU plus matrix baud."""
    if baud is None or str(baud) == '115200':
        return f'{flavor}-{board_mcu(board)}'
    return f'{flavor}-{board_mcu(board)}-{baud}'


def record_peak(label, peak):
    """Keep the max painted peak per firmware in the run's JSON file.

    A run without R2S_E2E_AVR_STACK_OUT records nothing, so standalone
    launches and unit tests stay file-free. A broken file restarts fresh
    with a warning on stderr; the table step reports which firmware never
    measured instead of failing the scenarios it summarizes.
    """
    path = os.environ.get('R2S_E2E_AVR_STACK_OUT', '')
    if not path:
        return
    try:
        stored = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        print(f'avr: cannot read {path}: {error}; recording fresh', file=sys.stderr)
        stored = {}
    if not isinstance(stored, dict):
        print(f'avr: {path} holds no mapping; recording fresh', file=sys.stderr)
        stored = {}
    prior = stored.get(label, 0)
    if not isinstance(prior, int) or isinstance(prior, bool):
        prior = 0
    stored[label] = max(prior, peak)
    try:
        Path(path).write_text(json.dumps(stored, indent=2) + '\n', encoding='utf-8')
    except OSError as error:
        print(f'avr: cannot write {path}: {error}', file=sys.stderr)


def harness_argv(board, flavor='slice', baud=None):
    """The r2s_simavr command for one firmware, always painting the stack."""
    return [
        str(harness_binary()),
        '--mcu',
        board_mcu(board),
        '--firmware',
        str(firmware_image(board, flavor, baud)),
        *RUN_MS,
        '--stack-paint',
    ]


class Spawned:
    """A running harness, with the pty path and the time it was announced."""

    def __init__(self, proc, pty, ready_at, label=''):
        self.proc = proc
        self.pty = pty
        self.ready_at = ready_at
        self.label = label
        self.returncode = None
        self.summary = {}

    def stop(self):
        """Terminate the harness, keeping its stop summary for the rate gate."""
        if self.returncode is not None:
            return self.returncode
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            tail, _ = self.proc.communicate(timeout=10.0)
        except subprocess.TimeoutExpired as expired:
            self.proc.kill()
            tail, _ = self.proc.communicate()
            raise AssertionError('the harness ignored SIGTERM') from expired
        self.returncode = self.proc.returncode
        for line in tail.decode().splitlines():
            if line.startswith('r2s_simavr: stop '):
                self.summary = parse_stop_summary(line + '\n')
        if self.label and 'stack_peak' in self.summary:
            record_peak(self.label, self.summary['stack_peak'])
        return self.returncode


def _await_pty(proc, deadline):
    pending = b''
    while time.monotonic() < deadline:
        readable, _, _ = select.select([proc.stdout.fileno()], [], [], 0.5)
        if proc.stdout.fileno() in readable:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if chunk:
                pending += chunk
        newline = pending.find(b'\n')
        while newline >= 0:
            line, pending = pending[:newline].decode(), pending[newline + 1 :]
            if line.startswith('r2s_simavr: pty='):
                return parse_pty_line(line + '\n'), time.monotonic()
            newline = pending.find(b'\n')
        if proc.poll() is not None:
            raise AssertionError('the harness exited before its pty line')
    raise AssertionError(f'the harness announced no pty within {READY_TIMEOUT:.0f} s')


def spawn(board, flavor='slice', baud=None):
    """Start the board's firmware under simavr and return it once booted."""
    proc = subprocess.Popen(
        harness_argv(board, flavor, baud),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    try:
        pty, ready_at = _await_pty(proc, time.monotonic() + READY_TIMEOUT)
    except BaseException:
        proc.kill()
        raise
    return Spawned(proc, pty, ready_at, firmware_label(board, flavor, baud))
