# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The nightly mutation run keeps its reports inside the report directory.

Mull composes a report path as "<report-dir>/<report-name>.sqlite". The
runner gave it an absolute path as the name, so the composed path was
"<cwd>/<absolute path>", the SQLite open failed after every mutant, and the
nightly job went red on a directory it could have refused in the first
second. The real Mull binary is only in the mutation image, so the stubs
here implement that documented join: what the runner passes is what the
report path becomes.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / 'scripts' / 'run-mutation.sh'

# A stub that records its arguments, so a test can see which path the run
# would have written.
RECORD = """#!/bin/bash
printf '%s %s\\n' "${0##*/}" "$*" >>"$STUB_CALLS"
"""

# Mull joins the directory and the bare name; this mirrors its reporter.
MULL = (
    RECORD
    + """
dir=.
name=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --report-dir) dir=$2; shift 2 ;;
    --report-name) name=$2; shift 2 ;;
    *) shift ;;
  esac
done
case "$0" in
  *mull-runner*) touch "$dir/$name.sqlite" || exit 1 ;;
  *) printf '{"files": {}}' >"$dir/$name.json" || exit 1 ;;
esac
"""
)

CMAKES = (
    RECORD
    + """
while [ "$#" -gt 0 ]; do
  case "$1" in
    -B) build=$2; shift 2 ;;
    *) shift ;;
  esac
done
if [ -n "${build:-}" ]; then
  mkdir -p "$build/tests"
  printf '#!/bin/bash\\nexit 0\\n' >"$build/tests/test_foo"
  chmod +x "$build/tests/test_foo"
fi
"""
)

CTEST = """#!/bin/bash
echo 'Test project /tmp/build'
echo '  Test   #1: test_foo.test_bar'
echo '  Test   #2: test_foo.test_baz'
"""

# The compiler stub satisfies build-host-lib.sh without compiling it: this
# test is about report paths, not about the host library.
CC = """#!/bin/bash
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift 2 ;;
    *) shift ;;
  esac
done
touch "${out:?}"
"""

# The run stops here: the generator stage is not what this test measures,
# and the alternative is writing mutmut's state into the tree.
MUTMUT = """#!/bin/bash
case "$1" in
  export-cicd-stats) echo stub-mutmut-stop; exit 1 ;;
esac
exit 0
"""


def write_stubs(bindir):
    stubs = {
        'mull-runner': MULL,
        'mull-reporter': MULL,
        'cmake': CMAKES,
        'ctest': CTEST,
        'cc': CC,
        'mutmut': MUTMUT,
        'ninja': '#!/bin/bash\nexit 0\n',
    }
    for name, body in stubs.items():
        (bindir / name).write_text(body)
        (bindir / name).chmod(0o755)


def run_runner(bindir, report_dir, calls_file):
    return subprocess.run(
        ['bash', str(RUNNER)],
        capture_output=True,
        text=True,
        cwd=ROOT,
        timeout=300,
        env={
            **os.environ,
            'PATH': f'{bindir}:{os.environ["PATH"]}',
            'R2S_MUTATION_OUT': str(report_dir),
            'STUB_CALLS': str(calls_file),
        },
    )


def reports_in(calls_file, report_dir):
    """The report paths the stubs would have written, as paths."""
    written = []
    for line in calls_file.read_text().splitlines():
        if line.startswith(('mull-runner', 'mull-reporter')):
            written.append(line)
    assert written, calls_file.read_text()
    return written


def test_every_report_lands_in_the_report_directory(tmp_path):
    """Mull's composed path is the runner's report directory, never the cwd."""
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    report_dir = tmp_path / 'report'
    calls = tmp_path / 'calls'
    result = run_runner(bindir, report_dir, calls)
    assert 'report directory is not usable' not in result.stderr, result.stderr
    assert reports_in(calls, report_dir)
    landed = sorted(path.name for path in report_dir.glob('*.sqlite')) + sorted(
        path.name for path in report_dir.glob('mull*.json')
    )
    assert 'mull.sqlite' in landed, landed
    assert 'mull-bridge.sqlite' in landed, landed
    assert 'mull.json' in landed, landed
    assert 'mull-bridge.json' in landed, landed
    assert not (ROOT / 'mutation' / 'bridge' / 'mull-bridge.sqlite').exists()


def test_the_run_reaches_the_generator_stage_that_stops_the_stub(tmp_path):
    """The stub stops at the generator, so the Mull stages all ran.

    A silent early exit would make the path assertions above vacuous.
    """
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    result = run_runner(bindir, tmp_path / 'report', tmp_path / 'calls')
    assert result.returncode != 0
    assert 'stub-mutmut-stop' in (result.stdout + result.stderr), result.stdout


def test_the_mutation_run_refuses_a_report_dir_that_is_a_file(tmp_path):
    """A path that cannot hold the reports fails before any tool is called.

    811 mutants ran before the old run noticed, so the directory check has
    to come first and name the path it refused.
    """
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    blocker = tmp_path / 'not-a-directory'
    blocker.write_text('')
    calls = tmp_path / 'calls'
    result = run_runner(bindir, blocker, calls)
    assert result.returncode != 0, result.stdout
    assert str(blocker) in result.stderr, result.stderr
    assert 'report directory' in result.stderr, result.stderr
    assert not calls.exists(), calls.read_text() if calls.exists() else ''


@pytest.mark.skipif(os.geteuid() == 0, reason='root ignores directory write permissions')
def test_the_mutation_run_refuses_an_unwritable_report_dir(tmp_path):
    """A directory the run cannot write fails before any tool is called."""
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    locked = tmp_path / 'locked'
    locked.mkdir()
    locked.chmod(0o555)
    calls = tmp_path / 'calls'
    try:
        result = run_runner(bindir, locked, calls)
    finally:
        locked.chmod(0o755)
    assert result.returncode != 0, result.stdout
    assert str(locked) in result.stderr, result.stderr
    assert 'report directory' in result.stderr, result.stderr
    assert not calls.exists(), calls.read_text() if calls.exists() else ''
