# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The generator stage runs mutmut in a workspace shaped the way it needs.

mutmut names a mutant after the source path it was given and runs pytest
from mutants/. So the package has to sit at the top of the directory mutmut
runs in, and the suite has to be staged beside it. It also spells its
result flag as a value option, so a bare --all exits 2. These tests drive
run-mutation.sh with stubs that model all four facts, and fail when any of
them drifts.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / 'scripts' / 'run-mutation.sh'

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
"""

# mutmut, modelled from 3.8.1: the workspace must carry r2s_gen at its top
# and a gen/tests inside mutants/; --all is a value option.
MUTMUT = (
    RECORD
    + """
cwd=$(pwd)
case "$1" in
  run)
    [ -d "$cwd/r2s_gen" ] || { echo "no r2s_gen at the workspace top: $cwd" >&2; exit 9; }
    [ -d "$cwd/mutants/gen/tests" ] || { echo "no suite in mutants/" >&2; exit 9; }
    grep -q 'source_paths = \\["r2s_gen"\\]' "$cwd/pyproject.toml" || {
      echo "the config does not name the top-level package" >&2
      exit 9
    }
    mkdir -p mutants
    printf '{"killed": 1}' >mutants/mutmut-cicd-stats.json
    ;;
  export-cicd-stats) [ -f mutants/mutmut-cicd-stats.json ] || exit 9 ;;
  results)
    [ "${2:-}" = --all ] && [ "${3:-}" = true ] || { echo "results needs --all true" >&2; exit 9; }
    ;;
esac
exit 0
"""
)

CC = """#!/bin/bash
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift 2 ;;
    *) shift ;;
  esac
done
touch "${out:?}"
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
        timeout=600,
        env={
            **os.environ,
            'PATH': f'{bindir}:{os.environ["PATH"]}',
            'R2S_MUTATION_OUT': str(report_dir),
            'STUB_CALLS': str(calls_file),
        },
    )


def report_dirs(calls_file):
    """Every Mull call's report directory and bare report name."""
    lines = [
        line
        for line in calls_file.read_text().splitlines()
        if line.startswith(('mull-runner', 'mull-reporter'))
    ]
    assert lines, calls_file.read_text()
    dirs = []
    for line in lines:
        fields = line.split()
        assert '--report-dir' in fields, line
        dirs.append(fields[fields.index('--report-dir') + 1])
        name = fields[fields.index('--report-name') + 1]
        assert '/' not in name, line
    return dirs


def test_every_report_lands_in_the_report_directory(tmp_path):
    """Mull's composed path is the runner's report directory, never the cwd."""
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    report_dir = tmp_path / 'report'
    calls = tmp_path / 'calls'
    result = run_runner(bindir, report_dir, calls)
    assert 'report directory is not usable' not in result.stderr, result.stderr
    dirs = report_dirs(calls)
    assert set(dirs) == {str(report_dir)}, dirs
    # The bridge loop once left its database beside its own mull.yml, outside
    # the directory the job uploads; the run must not recreate it.
    stray = ROOT / 'mutation' / 'bridge' / 'mull-bridge.sqlite'
    existed = stray.exists()
    landed = sorted(path.name for path in report_dir.glob('*.sqlite')) + sorted(
        path.name for path in report_dir.glob('mull*.json')
    )
    assert 'mull.sqlite' in landed, landed
    assert 'mull-bridge.sqlite' in landed, landed
    assert 'mull.json' in landed, landed
    assert 'mull-bridge.json' in landed, landed
    assert stray.exists() == existed, f'the run wrote {stray}'


def test_the_generator_stage_reaches_its_end(tmp_path):
    """Every mutmut call the runner makes must succeed in the staged workspace.

    The stub fails on the layout or the flag the runner gets wrong, so a
    regression here names the step instead of the nightly.
    """
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    write_stubs(bindir)
    report_dir = tmp_path / 'report'
    calls = tmp_path / 'calls'
    result = run_runner(bindir, report_dir, calls)
    mutmut_calls = [line for line in calls.read_text().splitlines() if line.startswith('mutmut ')]
    assert any(line.startswith('mutmut run') for line in mutmut_calls), mutmut_calls
    assert any('export-cicd-stats' in line for line in mutmut_calls), mutmut_calls
    assert any('results --all true' in line for line in mutmut_calls), mutmut_calls
    assert 'no r2s_gen' not in result.stderr, result.stderr
    assert 'no suite in mutants/' not in result.stderr, result.stderr
    assert 'results needs' not in result.stderr, result.stderr
    assert (report_dir / 'mutmut-cicd-stats.json').is_file()


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
