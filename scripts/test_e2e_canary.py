"""The e2e runners wait for the canary to show before running scenarios.

await_canary blocks until the iteration's canary lists in its own graph,
so sweeps never judge an iteration whose canary is still undiscovered.
These tests run the runners' own function with a stubbed ros2, never
a suite.
"""

import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUNNERS = {
    "posix": ROOT / "tests" / "e2e" / "run.sh",
    "avr": ROOT / "tests" / "e2e" / "run-avr.sh",
}


def await_function(path):
    """The await_canary function exactly as the runner carries it."""
    lines = path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line == "await_canary() {")
    end = next(i for i in range(start, len(lines)) if lines[i] == "}")
    return "\n".join(lines[start : end + 1])


def run_gate(path, ros_body, extra=""):
    """await_canary against a stubbed ros2 CLI; canary_topic is always set."""
    script = "set -u\ncleanup_and_exit() { echo BAILED; exit 1; }\nsleep() { :; }\n"
    script += "ros2() {\n" + ros_body + "\n}\n"
    script += "rmw=rmw_fastrtps_cpp\ncanary_topic=r2s_canary_test\n" + extra
    script += await_function(path) + "\nawait_canary\n"
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True)


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_visible_canary_proceeds(runner, tmp_path):
    proc = run_gate(RUNNERS[runner], 'echo "/diagnostics"; echo "/r2s_canary_test"')
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_hidden_canary_retries_until_seen(runner, tmp_path):
    seen = tmp_path / "calls"
    seen.write_text("0")
    ros_body = (
        f"n=$(cat {shlex.quote(str(seen))}); echo $((n + 1)) > {shlex.quote(str(seen))}\n"
        'echo "/diagnostics"\nif [ "$n" -ge 2 ]; then echo "/r2s_canary_test"; fi'
    )
    proc = run_gate(RUNNERS[runner], ros_body)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout
    assert int(seen.read_text()) >= 3


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_a_missing_canary_fails_loudly_on_a_bound(runner):
    body = await_function(RUNNERS[runner])
    assert "SECONDS + 180" in body
    assert "never appeared" in body and "cleanup_and_exit" in body
