"""The simavr backend: the slice sketch on an emulated Uno or Mega.

Each scenario spawns the board's ELF under r2s_simavr and talks to the pty
the harness announces, reusing the bridge launcher and the shared checks.
The harness binary and the firmware directory come from tests/e2e/run-avr.sh,
which builds both; R2S_E2E_BOARD picks the board for a standalone run.
"""

import os
import select
import subprocess
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
READY_TIMEOUT = 20.0
RUN_MS = ("--run-ms", "300000")

_BOARDS = {
    "uno": ("atmega328p", "arduino:avr:uno"),
    "mega": ("atmega2560", "arduino:avr:mega"),
}


def board_mcu(board):
    """The simavr MCU name for the board, rejecting unknown boards."""
    assert board in _BOARDS, f"unknown avr board {board!r}: want one of {sorted(_BOARDS)}"
    return _BOARDS[board][0]


def board_fqbn(board):
    """The arduino-cli board name the slice sketch compiles for."""
    assert board in _BOARDS, f"unknown avr board {board!r}: want one of {sorted(_BOARDS)}"
    return _BOARDS[board][1]


def parse_pty_line(line):
    """The pty slave path from the harness announcement line."""
    assert line.startswith("r2s_simavr: pty="), "no pty in harness line: " + line.strip()
    return line.strip().split("=", 1)[1]


def harness_binary():
    """The r2s_simavr binary, built by tests/e2e/run-avr.sh."""
    given = os.environ.get("R2S_SIMAVR_BIN", "")
    assert given, "R2S_SIMAVR_BIN is unset: run tests/e2e/run-avr.sh, which builds the harness"
    assert Path(given).is_file(), "no harness at " + given + ": run tests/e2e/run-avr.sh"
    return Path(given)


def firmware_image(board):
    """The board's slice ELF, built by tests/e2e/run-avr.sh."""
    given = os.environ.get("R2S_E2E_AVR_FW", "")
    assert given, "R2S_E2E_AVR_FW is unset: run tests/e2e/run-avr.sh, which builds the firmware"
    path = Path(given) / f"slice-{board_mcu(board)}.elf"
    assert path.is_file(), "no firmware at " + str(path) + ": run tests/e2e/run-avr.sh"
    return path


class Spawned:
    """A running harness, with the pty path and the time it was announced."""

    def __init__(self, proc, pty, ready_at):
        self.proc = proc
        self.pty = pty
        self.ready_at = ready_at
        self.returncode = None

    def stop(self):
        """Terminate the harness, killing it when it ignores the signal."""
        if self.returncode is not None:
            return self.returncode
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.communicate(timeout=10.0)
        except subprocess.TimeoutExpired as expired:
            self.proc.kill()
            self.proc.communicate()
            raise AssertionError("the harness ignored SIGTERM") from expired
        self.returncode = self.proc.returncode
        return self.returncode


def _await_pty(proc, deadline):
    pending = b""
    while time.monotonic() < deadline:
        readable, _, _ = select.select([proc.stdout.fileno()], [], [], 0.5)
        if proc.stdout.fileno() in readable:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if chunk:
                pending += chunk
        newline = pending.find(b"\n")
        while newline >= 0:
            line, pending = pending[:newline].decode(), pending[newline + 1 :]
            if line.startswith("r2s_simavr: pty="):
                return parse_pty_line(line + "\n"), time.monotonic()
            newline = pending.find(b"\n")
        if proc.poll() is not None:
            raise AssertionError("the harness exited before its pty line")
    raise AssertionError(f"the harness announced no pty within {READY_TIMEOUT:.0f} s")


def spawn(board):
    """Start the board's firmware under simavr and return it once booted."""
    proc = subprocess.Popen(
        [str(harness_binary()), "--mcu", board_mcu(board), "--firmware", str(firmware_image(board)), *RUN_MS],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    try:
        pty, ready_at = _await_pty(proc, time.monotonic() + READY_TIMEOUT)
    except BaseException:
        proc.kill()
        raise
    return Spawned(proc, pty, ready_at)
