"""The e2e runners clear each RMW's barrier slots after the exit rendezvous.

barrier_clear forgets one RMW's four slot files once both sides met at its
exit barrier, so every leg and pass re-rendezvouses instead of only the
first. These tests run the runners' own function with stubbed teardown,
never a suite.
"""

import os
import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUNNERS = (ROOT / "tests" / "e2e" / "run.sh", ROOT / "tests" / "e2e" / "run-avr.sh")


def clear_function(path):
    """The barrier_clear function exactly as the runner carries it."""
    lines = path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line == "barrier_clear() {")
    end = next(i for i in range(start, len(lines)) if lines[i] == "}")
    return "\n".join(lines[start : end + 1])


def run_clear(path, barrier, slot="first", peer="second", name="rmw_fastrtps_cpp"):
    """barrier_clear from the runner against a throwaway barrier dir."""
    script = "set -u\ncleanup_and_exit() { echo BAILED; exit 1; }\n"
    script += "".join(
        f"{key}={shlex.quote(value)}\n"
        for key, value in (("R2S_E2E_BARRIER", barrier), ("R2S_E2E_SLOT", slot), ("R2S_E2E_PEER", peer))
    )
    script += clear_function(path) + f"\nbarrier_clear {shlex.quote(name)}\n"
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, env={"PATH": os.environ["PATH"]})


@pytest.mark.parametrize("runner", RUNNERS, ids=("posix", "avr"))
def test_clear_forgets_this_rmw_only(runner, tmp_path):
    for name in ("first-x", "second-x", "first-x-done", "second-x-done", "first-y"):
        (tmp_path / name).touch()
    proc = run_clear(runner, str(tmp_path), name="x")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout
    remaining = sorted(path.name for path in tmp_path.iterdir())
    assert remaining == ["first-y"], f"clear left or removed the wrong files: {remaining}"


@pytest.mark.parametrize("runner", RUNNERS, ids=("posix", "avr"))
def test_clear_is_a_noop_outside_the_pairs(runner, tmp_path):
    """Standalone runs set no barrier, so clear must not even read the slots."""
    script = "set -u\ncleanup_and_exit() { echo BAILED; exit 1; }\n"
    script += clear_function(runner) + "\nbarrier_clear x\n"
    proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env={"PATH": os.environ["PATH"]})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout
