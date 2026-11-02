"""The e2e runners wait for the canary to show before running scenarios.

await_canary blocks until the iteration's canary lists in its own graph,
so sweeps never judge an iteration whose canary is still undiscovered.
These tests run the runners' own function against a fake ros2 binary on
PATH (a shell function would be invisible through env), never a suite.
"""

import os
import shlex
import stat
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


def run_gate(path, show_after, record):
    """await_canary against a fake ros2 that lists the canary from one call on."""
    bindir = record.parent / "bin"
    bindir.mkdir(exist_ok=True)
    counter = record.parent / "calls"
    counter.write_text("0")
    fake = bindir / "ros2"
    fake.write_text(
        "#!/bin/bash\n"
        f'echo "args: $*" >> {shlex.quote(str(record))}\n'
        'echo "router: ${ZENOH_ROUTER_CONFIG_URI-}" >> "$R2S_TEST_RECORD"\n'
        f"n=$(cat {shlex.quote(str(counter))}); echo $((n + 1)) > {shlex.quote(str(counter))}\n"
        'echo "/diagnostics"\n'
        f'if [ "$n" -ge {show_after} ]; then echo "/r2s_canary_test"; fi\n'
    )
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    script = "set -u\ncleanup_and_exit() { echo BAILED; exit 1; }\nsleep() { :; }\n"
    script += "rmw=rmw_fastrtps_cpp\ncanary_topic=r2s_canary_test\n"
    script += "ZENOH_ROUTER_CONFIG_URI=for-rmw-zenohd-only\n"
    script += await_function(path) + "\nawait_canary\n"
    env = {"PATH": str(bindir) + ":" + os.environ["PATH"], "R2S_TEST_RECORD": str(record)}
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env)


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_visible_canary_proceeds(runner, tmp_path):
    proc = run_gate(RUNNERS[runner], 0, tmp_path / "probe")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_hidden_canary_retries_until_seen(runner, tmp_path):
    record = tmp_path / "probe"
    proc = run_gate(RUNNERS[runner], 2, record)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "BAILED" not in proc.stdout
    assert sum(1 for line in record.read_text().splitlines() if line.startswith("args:")) >= 3


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_probe_skips_the_daemon_and_the_router_uri(runner, tmp_path):
    """The warm-up probe matches the sweep probe: no daemon, no router URI."""
    record = tmp_path / "probe"
    proc = run_gate(RUNNERS[runner], 0, record)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    lines = record.read_text().splitlines()
    assert "topic list --no-daemon" in lines[0], lines
    assert lines[1] == "router: ", lines


@pytest.mark.parametrize("runner", sorted(RUNNERS), ids=sorted(RUNNERS))
def test_a_missing_canary_fails_loudly_on_a_bound(runner):
    body = await_function(RUNNERS[runner])
    assert "SECONDS + 180" in body
    assert "never appeared" in body and "cleanup_and_exit" in body
