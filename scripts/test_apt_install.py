# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""scripts/apt-install.sh retries transient apt failures, then fails loud.

The dev image, the rosdoc2 job, the farm job and the e2e-posix legs all
install through this helper, so one transient index failure costs seconds
instead of a whole rerun. These tests drive the helper with a stub apt-get
on PATH: no network, no root, no real package lists.
"""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / 'scripts' / 'apt-install.sh'

STUB = """#!/usr/bin/env bash
# A stub apt-get: counts its runs, records its argv, then fails until
# the count passes FAIL_UNTIL. Every line it prints is the "apt output"
# the helper must match or surface.
count=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" >"$COUNT_FILE"
printf '%s\\n' "$*" >>"$ARGV_LOG"
echo "$FAIL_LINE (attempt $count)"
if [ "$count" -le "$FAIL_UNTIL" ]; then
    exit "$FAIL_RC"
fi
exit 0
"""


def run_helper(bindir, stub_env, *args):
    """Run the helper with the stub bin dir first on PATH, no backoff."""
    env = dict(os.environ)
    env['PATH'] = str(bindir) + ':' + env['PATH']
    env['R2S_APT_RETRY_DELAY'] = '0'
    env.update(stub_env)
    return subprocess.run(
        ['bash', str(HELPER), *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )


def make_stub(tmp_path, fail_until, rc=100, line='boom'):
    """A bin dir whose `apt-get` fails until past the fail_until-th run."""
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub = bindir / 'apt-get'
    stub.write_text(STUB)
    stub.chmod(0o755)
    count = tmp_path / 'count'
    argv_log = tmp_path / 'argv'
    argv_log.write_text('')
    stub_env = {
        'COUNT_FILE': str(count),
        'ARGV_LOG': str(argv_log),
        'FAIL_UNTIL': str(fail_until),
        'FAIL_RC': str(rc),
        'FAIL_LINE': line,
    }
    return bindir, count, argv_log, stub_env


def attempts(count_file):
    """How many times the stub apt-get ran."""
    return int(count_file.read_text().strip())


def test_transient_fetch_failure_is_retried_then_succeeds(tmp_path):
    """A `Failed to fetch` on the first run succeeds on the second."""
    bindir, count, argv_log, stub_env = make_stub(
        tmp_path,
        fail_until=1,
        line='E: Failed to fetch http://archive.ubuntu.com/ 500 Error',
    )
    result = run_helper(bindir, stub_env, 'install', '-y', 'some-package')
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 2, 'the transient failure was not retried'
    assert 'Acquire::Retries=3' in argv_log.read_text(), (
        'the helper must pass apt its own per-fetch retries'
    )


def test_persistent_failure_surfaces_the_real_error(tmp_path):
    """A package that never exists fails bounded, with apt's own error."""
    line = "E: Package 'gone' has no installation candidate"
    bindir, count, argv_log, stub_env = make_stub(tmp_path, fail_until=99, line=line)
    result = run_helper(bindir, stub_env, 'install', '-y', 'gone')
    assert result.returncode != 0, 'a missing package reported success'
    assert attempts(count) == 3, 'the attempts were not bounded at 3'
    assert line in result.stdout + result.stderr, 'the last apt error never reached the log'


def test_missing_subcommand_is_a_usage_error(tmp_path):
    """No apt subcommand exits 2 the way the other script helpers do."""
    bindir, _, _, stub_env = make_stub(tmp_path, fail_until=0)
    result = run_helper(bindir, stub_env)
    assert result.returncode == 2, result.stderr
