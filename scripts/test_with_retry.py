"""with-retry.sh reruns a command until it passes, bounded and reporting.

The readme echo runs its exact walkthrough command through this helper, so
a one-shot miss under load retries instead of failing the leg. These tests
drive the helper with stub commands that count their own runs: no ROS, no
bridge, no timing dependence beyond generous bounds.
"""

import os
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / "scripts" / "with-retry.sh"

STUB = """#!/bin/bash
# A stub command: fails until its counter reaches SUCCEED_ON, then prints.
count=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" >"$COUNT_FILE"
echo "attempt $count says: $STUB_LINE"
if [ "$count" -lt "$SUCCEED_ON" ]; then
    exit "$STUB_RC"
fi
exit 0
"""


def run_helper(stub_dir, *args, cwd=None):
    """Run the helper with the stub bin dir first on PATH."""
    env = {"PATH": str(stub_dir) + ":" + os.environ["PATH"]}
    return subprocess.run(
        ["bash", str(HELPER), *args],
        capture_output=True,
        text=True,
        env=env,
        cwd=cwd or stub_dir,
        timeout=90,
    )


def make_stub(tmp_path, succeed_on=1, rc=3, line="boom"):
    """A bin dir whose `probe` fails until the succeed_on-th run."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "probe").write_text(STUB)
    (bindir / "probe").chmod(0o755)
    count = tmp_path / "count"
    env = {"COUNT_FILE": str(count), "SUCCEED_ON": str(succeed_on)}
    starter = dict(env)
    starter.update({"STUB_RC": str(rc), "STUB_LINE": line})
    (tmp_path / "env_probe").write_text("")
    return bindir, count, starter


def attempts(count_file):
    """How many times the stub ran."""
    return int(count_file.read_text().strip())


def test_success_first_try_runs_once(tmp_path):
    """The happy path costs exactly one attempt."""
    bindir, count, env = make_stub(tmp_path, succeed_on=1)
    out = tmp_path / "out.txt"
    result = run_helper(
        bindir,
        "--tries", "3", "--timeout", "10", "--out", str(out), "--",
        "env", *[f"{k}={v}" for k, v in env.items()], "probe",
    )
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 1, "a passing command ran more than once"
    assert "attempt 1 says" in out.read_text(), out.read_text()


def test_flaky_then_success_reports_the_winning_state(tmp_path):
    """Two misses then a reading: green, with three attempts on record."""
    bindir, count, env = make_stub(tmp_path, succeed_on=3, line="data: 1.0")
    out = tmp_path / "out.txt"
    result = run_helper(
        bindir,
        "--tries", "3", "--timeout", "10", "--out", str(out), "--",
        "env", *[f"{k}={v}" for k, v in env.items()], "probe",
    )
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 3, "the helper gave up early or ran long"
    assert "data: 1.0" in out.read_text(), out.read_text()


def test_total_failure_is_bounded_and_reports_the_last_state(tmp_path):
    """A real regression stays visible: tries bounded, last output kept."""
    bindir, count, env = make_stub(tmp_path, succeed_on=99, rc=7, line="empty graph")
    out = tmp_path / "out.txt"
    result = run_helper(
        bindir,
        "--tries", "3", "--timeout", "10", "--out", str(out), "--",
        "env", *[f"{k}={v}" for k, v in env.items()], "probe",
    )
    assert result.returncode != 0, "a failing command reported success"
    assert attempts(count) == 3, "the attempts were not bounded at tries"
    assert "attempt 3 says: empty graph" in out.read_text(), out.read_text()
    assert "attempt 3 says: empty graph" in result.stderr, result.stderr


def test_a_hanging_command_is_cut_off_per_attempt(tmp_path):
    """A wedged attempt cannot eat the whole budget alone."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    (bindir / "probe").write_text("#!/bin/bash\nsleep 30\n")
    (bindir / "probe").chmod(0o755)
    out = tmp_path / "out.txt"
    start = time.monotonic()
    result = run_helper(
        bindir, "--tries", "2", "--timeout", "2", "--out", str(out), "--", "probe"
    )
    elapsed = time.monotonic() - start
    assert result.returncode != 0, "a hanging command reported success"
    assert elapsed < 20, f"the per-attempt timeout did not bound the run: {elapsed:.1f}s"


def test_missing_command_is_a_usage_error(tmp_path):
    """No command to run exits 2 the way the other script helpers do."""
    result = run_helper(
        tmp_path,
        "--tries", "3", "--timeout", "10", "--out", str(tmp_path / "out.txt"),
        cwd=tmp_path,
    )
    assert result.returncode == 2, result.stderr
