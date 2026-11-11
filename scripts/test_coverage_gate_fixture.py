# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The coverage gate judges a fixture PR: fail on an uncovered changed line.

A dip of the whole-package figure passes when every changed reachable line is
covered or carries a reasoned exclusion, and fails naming the file and line
otherwise. Floors still judge the whole package.

The fixture repo carries the real scripts/coverage-gate.sh with the real
scripts/e2e-lease.sh behind its isolation lease, the real
scripts/coverage-filters.sh and the real tools/coverage_gate.py, with stub
builds (core-test.sh, colcon) and a stub gcovr that reports the figures its
environment names. Nothing here compiles or measures real code.
"""

import json
import os
import re
import shutil
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GCOVR_STUB = """#!/usr/bin/env bash
set -uo pipefail
root=""
summary=""
detail=""
gotfilter=0
all_args="$*"
while [ $# -gt 0 ]; do
  case "$1" in
    --root) root="$2"; shift 2 ;;
    --json-summary=*) summary="${1#--json-summary=}"; shift ;;
    --json=*) detail="${1#--json=}"; shift ;;
    --filter) gotfilter=1; shift 2 ;;
    *) shift ;;
  esac
done
[ "$gotfilter" = 1 ] || exit 3
[ -n "$summary" ] && [ -n "$detail" ] || exit 3
if [ -n "${R2S_GCOVR_LOG:-}" ]; then printf '%s\n' "$all_args" >>"$R2S_GCOVR_LOG"; fi
if [[ "$root" == */base ]]; then figs=$R2S_FIG_BASE; src=$R2S_DETAIL_BASE;
else figs=$R2S_FIG_HEAD; src=$R2S_DETAIL_HEAD; fi
set -- $figs
python3 -c "import json; json.dump({'line_covered': $1, 'line_total': 100, \
  'branch_covered': $2, 'branch_total': 100}, open('$summary', 'w'))"
cp "$src" "$detail"
"""
COLCON_STUB = """#!/usr/bin/env bash
set -uo pipefail
want=""
install=""
for arg in "$@"; do
  if [ "$want" = dir ]; then mkdir -p "$arg"; install="$arg"; want=""; fi
  if [ "$arg" = --build-base ] || [ "$arg" = --install-base ]; then want=dir; fi
done
touch "$install/setup.bash"
"""
COVERAGE_STUB = """#!/usr/bin/env bash
set -uo pipefail
all_args="$*"
if [ -n "${R2S_COVERAGE_LOG:-}" ]; then printf '%s\\n' "$all_args" >>"$R2S_COVERAGE_LOG"; fi
if [ "${1:-}" = run ]; then exit 0; fi
if [ "${1:-}" = json ]; then
  out=""
  while [ $# -gt 0 ]; do
    case "$1" in -o) out="$2"; shift 2 ;; *) shift ;; esac
  done
  [ -n "$out" ] || exit 3
  if [[ "$PWD" == */base ]]; then src=$R2S_COV_DETAIL_BASE; else src=$R2S_COV_DETAIL_HEAD; fi
  cp "$src" "$out"
  exit 0
fi
exit 3
"""


def write_detail(path: Path, doc: dict) -> str:
    path.write_text(json.dumps(doc))
    return str(path)


def empty_gcovr_detail(path: Path) -> str:
    return write_detail(path, {'gcovr/format_version': '0.10', 'files': []})


def empty_coveragepy(path: Path, lines: str = '96 91') -> str:
    covered, branches = (int(part) for part in lines.split())
    return write_detail(
        path,
        {
            'meta': {'format': 3},
            'files': {},
            'totals': {
                'covered_lines': covered,
                'num_statements': 100,
                'covered_branches': branches,
                'num_branches': 100,
            },
        },
    )


def fixture(tmp_path: Path) -> tuple[Path, dict[str, str]]:
    repo = tmp_path / 'repo'
    (repo / 'scripts').mkdir(parents=True)
    (repo / 'tools').mkdir()
    (repo / 'bridge').mkdir()
    shutil.copy(ROOT / 'scripts' / 'coverage-gate.sh', repo / 'scripts' / 'coverage-gate.sh')
    shutil.copy(ROOT / 'scripts' / 'e2e-lease.sh', repo / 'scripts' / 'e2e-lease.sh')
    shutil.copy(ROOT / 'scripts' / 'coverage-filters.sh', repo / 'scripts' / 'coverage-filters.sh')
    shutil.copy(ROOT / 'tools' / 'coverage_gate.py', repo / 'tools' / 'coverage_gate.py')
    (repo / 'scripts' / 'core-test.sh').write_text('#!/usr/bin/env bash\nexit 0\n')
    (repo / 'scripts' / 'build-host-lib.sh').write_text('#!/usr/bin/env bash\nexit 0\n')
    (repo / 'bridge' / 'dummy.txt').write_text('bridge sources live here on a real tree\n')
    for name in (
        'scripts/coverage-gate.sh',
        'scripts/e2e-lease.sh',
        'scripts/core-test.sh',
        'scripts/build-host-lib.sh',
    ):
        path = repo / name
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    (bindir / 'gcovr').write_text(GCOVR_STUB)
    (bindir / 'colcon').write_text(COLCON_STUB)
    (bindir / 'coverage').write_text(COVERAGE_STUB)
    for name in ('gcovr', 'colcon', 'coverage'):
        path = bindir / name
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    git = ['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
    env = {'PATH': '/usr/bin:/bin', 'HOME': str(tmp_path)}
    subprocess.run(
        [*git, 'init', '-b', 'master'], cwd=repo, env=env, check=True, capture_output=True
    )
    subprocess.run([*git, 'add', '-A'], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run(
        [*git, 'commit', '-m', 'base'], cwd=repo, env=env, check=True, capture_output=True
    )
    subprocess.run(
        [*git, 'checkout', '--detach'], cwd=repo, env=env, check=True, capture_output=True
    )
    subprocess.run(
        [*git, 'commit', '--allow-empty', '-m', 'head'],
        cwd=repo,
        env=env,
        check=True,
        capture_output=True,
    )
    run_env = {
        'PATH': str(bindir),
        'HOME': str(tmp_path),
        'R2S_DETAIL_HEAD': empty_gcovr_detail(tmp_path / 'detail-head.json'),
        'R2S_DETAIL_BASE': empty_gcovr_detail(tmp_path / 'detail-base.json'),
        'R2S_COV_DETAIL_HEAD': empty_coveragepy(tmp_path / 'cov-head.json'),
        'R2S_COV_DETAIL_BASE': empty_coveragepy(tmp_path / 'cov-base.json'),
    }
    return repo, run_env


def run_gate(
    repo: Path,
    run_env: dict[str, str],
    head: str,
    base: str,
    extra: dict[str, str] | None = None,
    detail_head: str | None = None,
    detail_base: str | None = None,
    cov_head: str | None = None,
    cov_base: str | None = None,
) -> subprocess.CompletedProcess:
    env = {
        **os.environ,
        'PATH': f'{run_env["PATH"]}{os.pathsep}{os.environ.get("PATH", "/usr/bin:/bin")}',
        'HOME': run_env['HOME'],
        'ROS_DISTRO': 'jazzy',
        'AMENT_PREFIX_PATH': '/stub',
        'COMMIT_RANGE': 'master..HEAD',
        'R2S_FIG_HEAD': head,
        'R2S_FIG_BASE': base,
        'R2S_DETAIL_HEAD': detail_head or run_env['R2S_DETAIL_HEAD'],
        'R2S_DETAIL_BASE': detail_base or run_env['R2S_DETAIL_BASE'],
        'R2S_COV_DETAIL_HEAD': cov_head or run_env['R2S_COV_DETAIL_HEAD'],
        'R2S_COV_DETAIL_BASE': cov_base or run_env['R2S_COV_DETAIL_BASE'],
        **(extra or {}),
    }
    return subprocess.run(
        ['bash', 'scripts/coverage-gate.sh'], cwd=repo, env=env, capture_output=True, text=True
    )


def commit_on_head(repo: Path, run_env: dict[str, str], name: str, text: str) -> None:
    target = repo / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    git = ['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
    env = {'PATH': '/usr/bin:/bin', 'HOME': run_env['HOME']}
    subprocess.run([*git, 'add', '-A'], cwd=repo, env=env, check=True, capture_output=True)
    subprocess.run(
        [*git, 'commit', '-m', 'head'], cwd=repo, env=env, check=True, capture_output=True
    )


def test_the_gate_passes_when_head_dips_but_changed_lines_are_covered(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'core/src/f.c', 'int f(void) {\n  return 1;\n}\n')
    detail = write_detail(
        tmp_path / 'f-head.json',
        {
            'gcovr/format_version': '0.10',
            'files': [
                {
                    'file': 'core/src/f.c',
                    'functions': [],
                    'lines': [
                        {'line_number': 1, 'count': 2, 'branches': []},
                        {'line_number': 2, 'count': 2, 'branches': []},
                    ],
                }
            ],
        },
    )
    result = run_gate(repo, run_env, '96 91', '97 92', detail_head=detail)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'coverage: core patch lines 2/2 covered' in result.stdout


def test_the_gate_passes_when_head_matches_base(tmp_path):
    repo, run_env = fixture(tmp_path)
    result = run_gate(repo, run_env, '96 91', '96 91')
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_gate_uses_the_base_filters_on_head(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'scripts/coverage-filters.sh', 'this is not bash\n')
    result = run_gate(repo, run_env, '96 91', '96 91')
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_gate_excludes_throw_branches_from_every_measurement(tmp_path):
    repo, run_env = fixture(tmp_path)
    log = tmp_path / 'gcovr-args.log'
    result = run_gate(repo, run_env, '96 91', '96 91', {'R2S_GCOVR_LOG': str(log)})
    assert result.returncode == 0, result.stdout + result.stderr
    calls = log.read_text().splitlines()
    assert len(calls) == 4, 'one gcovr call per measurement:\n' + '\n'.join(calls)
    for call in calls:
        assert '--exclude-throw-branches' in call.split(), 'throw edges measured:\n' + call


def test_the_gate_requires_the_bridge_floors(tmp_path):
    repo, run_env = fixture(tmp_path)
    result = run_gate(repo, run_env, '71 51', '71 51')
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'coverage: bridge branch 51.00% is below the floor 80%' in result.stdout
    assert '(advisory)' not in result.stdout


def test_the_gate_passes_when_gen_dips_but_changed_lines_are_covered(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'gen/r2s_gen/m.py', 'def f(x):\n    return x\n')
    cov = write_detail(
        tmp_path / 'm-head.json',
        {
            'meta': {'format': 3},
            'files': {
                'gen/r2s_gen/m.py': {
                    'executed_lines': [1, 2],
                    'missing_lines': [],
                    'excluded_lines': [],
                    'executed_branches': [],
                    'missing_branches': [],
                    'summary': {},
                }
            },
            'totals': {
                'covered_lines': 95,
                'num_statements': 100,
                'covered_branches': 90,
                'num_branches': 100,
            },
        },
    )
    result = run_gate(repo, run_env, '96 91', '96 91', cov_head=cov)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'coverage: gen patch lines 2/2 covered' in result.stdout


def test_the_gate_measures_gen_branches(tmp_path):
    repo, run_env = fixture(tmp_path)
    log = tmp_path / 'coverage-args.log'
    result = run_gate(repo, run_env, '96 91', '96 91', {'R2S_COVERAGE_LOG': str(log)})
    assert result.returncode == 0, result.stdout + result.stderr
    calls = log.read_text().splitlines()
    assert any('--branch' in call.split() for call in calls), (
        'gen measured without branches:\n' + '\n'.join(calls)
    )


def test_the_gate_pins_the_gen_seed(tmp_path):
    repo, run_env = fixture(tmp_path)
    log = tmp_path / 'coverage-args.log'
    result = run_gate(repo, run_env, '96 91', '96 91', {'R2S_COVERAGE_LOG': str(log)})
    assert result.returncode == 0, result.stdout + result.stderr
    calls = log.read_text().splitlines()
    assert any('--hypothesis-seed=20261026' in call.split() for call in calls), (
        'gen measured without a pinned seed:\n' + '\n'.join(calls)
    )


def test_the_gate_runs_the_bridge_tests_under_a_lease(tmp_path):
    repo, run_env = fixture(tmp_path)
    result = run_gate(repo, run_env, '96 91', '96 91')
    assert result.returncode == 0, result.stdout + result.stderr
    match = re.search(r'coverage isolation: ROS_DOMAIN_ID=(\d+)', result.stdout)
    assert match is not None, 'the gate ran with no leased domain:\n' + result.stdout
    assert 0 <= int(match.group(1)) <= 31, 'the domain is outside the bridge pool range'


def gcovr_file_lines(rel, lines):
    return {
        'file': rel,
        'functions': [],
        'lines': [
            {'line_number': number, 'count': count, 'branches': []} for number, count in lines
        ],
    }


def test_the_gate_names_an_uncovered_changed_line(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'bridge/r2s_bridge/src/f.cpp', 'int f(void) {\n  return 1;\n}\n')
    detail = write_detail(
        tmp_path / 'uncovered-head.json',
        {
            'gcovr/format_version': '0.10',
            'files': [gcovr_file_lines('bridge/r2s_bridge/src/f.cpp', [(1, 2), (2, 0)])],
        },
    )
    result = run_gate(repo, run_env, '96 91', '96 91', detail_head=detail)
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'coverage: bridge patch: bridge/r2s_bridge/src/f.cpp:2: uncovered line' in result.stdout


def test_the_gate_rejects_an_exclusion_without_a_reason(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(
        repo,
        run_env,
        'bridge/r2s_bridge/src/g.cpp',
        'int g(void) {\n  return -1;  // GCOV_EXCL_LINE\n}\n',
    )
    detail = write_detail(
        tmp_path / 'bare-head.json',
        {
            'gcovr/format_version': '0.10',
            'files': [
                {
                    'file': 'bridge/r2s_bridge/src/g.cpp',
                    'functions': [],
                    'lines': [
                        {'line_number': 1, 'count': 2, 'branches': []},
                        {
                            'line_number': 2,
                            'count': 0,
                            'branches': [],
                            'gcovr/excluded': True,
                        },
                    ],
                }
            ],
        },
    )
    result = run_gate(repo, run_env, '96 91', '96 91', detail_head=detail)
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'excluded without a reason' in result.stdout
    assert 'bridge/r2s_bridge/src/g.cpp:2' in result.stdout


def test_the_gate_accepts_a_reasoned_exclusion(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(
        repo,
        run_env,
        'bridge/r2s_bridge/src/h.cpp',
        'int h(void) {\n  return -1;  // GCOV_EXCL_LINE: callers clamp\n}\n',
    )
    detail = write_detail(
        tmp_path / 'reasoned-head.json',
        {
            'gcovr/format_version': '0.10',
            'files': [
                {
                    'file': 'bridge/r2s_bridge/src/h.cpp',
                    'functions': [],
                    'lines': [
                        {'line_number': 1, 'count': 2, 'branches': []},
                        {
                            'line_number': 2,
                            'count': 0,
                            'branches': [],
                            'gcovr/excluded': True,
                        },
                    ],
                }
            ],
        },
    )
    result = run_gate(repo, run_env, '96 91', '96 91', detail_head=detail)
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'excluded' in result.stdout


def test_the_gate_ignores_changed_lines_outside_measured_code(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'docs/guide.md', '# Guide\n')
    commit_on_head(repo, run_env, 'gen/tests/golden/x.txt', 'golden\n')
    result = run_gate(repo, run_env, '96 91', '96 91')
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'outside measured code' in result.stdout
    assert 'docs/guide.md' in result.stdout


def test_the_gate_ignores_a_changed_file_with_no_coverage_data(tmp_path):
    repo, run_env = fixture(tmp_path)
    commit_on_head(repo, run_env, 'core/src/ghost.c', 'int g(void) {\n  return 1;\n}\n')
    result = run_gate(repo, run_env, '96 91', '96 91')
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'with no coverage data: core/src/ghost.c' in result.stdout
