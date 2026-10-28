"""The posix virtual MCU as a launched process.

The firmware prints a ready line once its port is open, just before it enters
the announce loop, so the launch time of that line stands in for the first
HELLO the scenarios measure from. A scratch boot file keeps parallel runs
from sharing the port's boot counter.
"""

import os
import select
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
BOOT_ENV = "R2S_POSIX_BOOT_ID_FILE"
READY_MARK = "r2s_posix: ready"
READY_TIMEOUT = 20.0


def build(scratch):
    """Build the posix firmware into scratch and return its path."""
    env = dict(os.environ)
    env["R2S_BUILD_DIR"] = str(scratch)
    proc = subprocess.run(
        ["bash", str(REPO_ROOT / "ports" / "posix" / "run.sh"), "--build-only"],
        capture_output=True,
        text=True,
        timeout=300,
        env=env,
        check=False,
    )
    if proc.returncode != 0:
        raise AssertionError("posix firmware build failed:\n" + proc.stderr[-4000:])
    firmware = Path(scratch) / "ports" / "posix" / "r2s_posix_firmware"
    if not firmware.is_file():
        raise AssertionError("posix firmware binary missing after build: " + str(firmware))
    return firmware


def from_run_script():
    """Return the firmware run.sh built, for tests launched by tests/e2e/run.sh."""
    given = os.environ.get("R2S_E2E_FW", "")
    assert given, "R2S_E2E_FW is unset: run tests/e2e/run.sh, which builds the firmware"
    assert Path(given).is_file(), "no firmware at " + given + ": run tests/e2e/run.sh"
    return Path(given)


class Spawned:
    """A running firmware with the monotonic time of its ready line."""

    def __init__(self, proc, boot_file, ready_at):
        self.proc = proc
        self.boot_file = boot_file
        self.ready_at = ready_at

    def stop(self):
        """Terminate the firmware, killing it when it ignores the signal."""
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5.0)


def _drain_until_ready(proc, deadline):
    pending = b""
    while time.monotonic() < deadline:
        readable, _, _ = select.select([proc.stderr], [], [], 0.5)
        if proc.stderr in readable:
            chunk = os.read(proc.stderr.fileno(), 4096)
            if chunk:
                pending += chunk
        if READY_MARK.encode() in pending:
            return time.monotonic()
        if proc.poll() is not None:
            raise AssertionError("firmware exited before its ready line")
    raise AssertionError(f"firmware printed no ready line within {READY_TIMEOUT:.0f} s")


def spawn(firmware, port, scratch, args=()):
    """Start the firmware on port and return it once its ready line lands."""
    boot_file = Path(scratch) / "boot"
    proc = subprocess.Popen(
        [str(firmware), port, *args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=False,
        env={**os.environ, BOOT_ENV: str(boot_file)},
    )
    try:
        ready_at = _drain_until_ready(proc, time.monotonic() + READY_TIMEOUT)
    except BaseException:
        proc.kill()
        raise
    return Spawned(proc, boot_file, ready_at)
