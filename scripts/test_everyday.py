# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The everyday make targets developers use next to the CI scopes."""

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run_make(*args, cwd=ROOT, env=None):
    merged = {**os.environ, **(env or {})}
    return subprocess.run(['make', *args], cwd=cwd, capture_output=True, text=True, env=merged)


def test_help_shows_the_everyday_group_first():
    out = run_make('help')
    assert out.returncode == 0, out.stderr
    assert 'Everyday' in out.stdout
    assert 'CI and agents' in out.stdout
    assert out.stdout.index('Everyday') < out.stdout.index('CI and agents')


def test_each_alias_expands_to_its_scope():
    aliases = {
        'test-core': 'CORE=1',
        'test-gen': 'GEN=1',
        'test-bridge': 'BRIDGE=1',
        'test-cpp': 'CPP=1',
        'test-scripts': 'SCRIPTS=1',
        'e2e-posix': 'E2E=posix',
        'e2e-avr': 'E2E=avr',
    }
    for alias, scope in aliases.items():
        out = run_make('-n', alias)
        assert out.returncode == 0, alias + out.stderr
        assert scope in out.stdout, alias
        assert ' test ' in out.stdout, alias


def test_test_all_expands_to_the_unscoped_suite():
    out = run_make('-n', 'test-all')
    assert out.returncode == 0, out.stderr
    assert re.search(r'\b(CORE|GEN|BRIDGE|CPP|SCRIPTS|LINT|E2E)=', out.stdout) is None


def test_check_expands_to_the_diff_scope_script():
    out = run_make('-n', 'check')
    assert out.returncode == 0, out.stderr
    assert 'scripts/check.sh' in out.stdout


def test_pytest_runs_a_single_selection():
    out = run_make('-n', 'pytest', 'ARGS=gen/test_x.py::test_y')
    assert out.returncode == 0, out.stderr
    assert 'scripts/pytest-one.sh' in out.stdout


def test_pytest_without_args_shows_usage():
    out = run_make('pytest')
    assert out.returncode != 0
    assert 'usage' in (out.stdout + out.stderr).lower()


def test_ctest_runs_a_filtered_build():
    out = run_make('-n', 'ctest', 'ARGS=rcobs')
    assert out.returncode == 0, out.stderr
    assert 'scripts/ctest-one.sh' in out.stdout


def test_ctest_without_args_shows_usage():
    out = run_make('ctest')
    assert out.returncode != 0
    assert 'usage' in (out.stdout + out.stderr).lower()


def test_misspelled_scope_fails_with_the_valid_scopes():
    out = run_make('-n', 'test', 'CROE=1')
    assert out.returncode != 0
    text = out.stdout + out.stderr
    assert 'CROE' in text
    assert 'CORE' in text


def test_lowercase_scope_suggests_the_uppercase_spelling():
    out = run_make('-n', 'test', 'core=1')
    assert out.returncode != 0
    assert 'CORE' in (out.stdout + out.stderr)


def test_valid_scope_names_match_the_suite():
    script = (ROOT / 'scripts/check-scope-vars.sh').read_text()
    scopes = set(re.search(r'^scopes="([^"]+)"', script, re.MULTILINE).group(1).split())
    wants = set(re.findall(r'wants ([A-Z][A-Z0-9_]*)', (ROOT / 'scripts/ci-test.sh').read_text()))
    assert scopes == wants | {'SCRIPTS_SHARD'}
    runner = (ROOT / 'scripts/run-tasks.sh').read_text()
    selected = set(re.findall(r'for scope in ([A-Z0-9_ ]+);', runner)[0].split())
    assert selected == wants - {'E2E'}
    assert 'e2e-posix' in runner and 'e2e-avr' in runner


def test_pytest_one_builds_the_host_lib_and_runs_pytest():
    text = (ROOT / 'scripts/pytest-one.sh').read_text()
    assert 'build-host-lib' in text
    assert 'python3 -m pytest' in text


def test_ctest_one_filters_a_built_config():
    text = (ROOT / 'scripts/ctest-one.sh').read_text()
    assert 'core-test.sh' in text
    assert '--build-only' in text
    assert re.search(r'ctest .* -R', text) is not None


def _build_default(text, leaf):
    return re.search(r'\$\{R2S_BUILD_DIR:-(.+?)\}/' + leaf, text).group(1)


def test_one_test_scripts_share_the_suite_build_dir_default():
    core = (ROOT / 'scripts/core-test.sh').read_text()
    one = (ROOT / 'scripts/ctest-one.sh').read_text()
    assert _build_default(one, 'core') == _build_default(core, 'core')
    suite = (ROOT / 'scripts/ci-test.sh').read_text()
    pytest_one = (ROOT / 'scripts/pytest-one.sh').read_text()
    assert _build_default(pytest_one, 'host') == _build_default(suite, 'host')


def git(repo, *args):
    subprocess.run(['git', *args], cwd=repo, check=True, capture_output=True, text=True)


def fixture_repo(path):
    repo = path / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'master')
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'core').mkdir()
    (repo / 'core' / 'a.c').write_text('int x;\n')
    git(repo, 'add', '.')
    git(repo, 'commit', '-q', '-m', 'feat(core): start')
    git(repo, 'update-ref', 'refs/remotes/origin/master', 'HEAD')
    return repo


STRIPPED_SCOPES = (
    'LINT',
    'SCRIPTS',
    'COMMITS',
    'CORE',
    'GEN',
    'BRIDGE',
    'AMENT_LINT',
    'CPP',
    'E2E',
    'COVERAGE',
    'DOCS',
    'EXAMPLES',
    'ESP32',
    'PIC',
    'BUDGETS',
    'FUZZ',
    'README',
    'NIGHTLY',
    'SOAK',
    'FULL',
    'R2S_FULL',
    'R2S_LOCAL',
    'SCRIPTS_SHARD',
)


def run_check(repo, out_file, **env):
    recorder = out_file.parent / 'recorder.sh'
    args_file = out_file.parent / 'args.txt'
    recorder.write_text(
        f'#!/usr/bin/env bash\necho "$*" > "{args_file}"\nenv | sort > "{out_file}"\n'
    )
    recorder.chmod(0o755)
    # One pass: the runner's own scopes must not leak in as caller scopes.
    clean = {k: v for k, v in os.environ.items() if k not in STRIPPED_SCOPES}
    clean.update(
        {'PATH': os.environ['PATH'], 'BASE': 'origin/master', 'R2S_SUITE': str(recorder), **env}
    )
    result = subprocess.run(
        ['bash', str(ROOT / 'scripts/check.sh')],
        cwd=repo,
        capture_output=True,
        text=True,
        env=clean,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    recorded = dict(
        line.split('=', 1)
        for line in out_file.read_text().splitlines()
        if re.match(r'^[A-Z][A-Z0-9_]*=', line)
    )
    return result.stdout, recorded, args_file.read_text().split()


def test_check_selects_the_caller_tasks(tmp_path):
    repo = fixture_repo(tmp_path)
    out_file = tmp_path / 'scopes.txt'
    stdout, recorded, args = run_check(repo, out_file, CORE='1')
    assert recorded.get('CORE') == '1'
    assert args == ['core'], args
    assert 'stale tasks' in stdout


def test_check_runs_unscoped_for_task_selection(tmp_path):
    repo = fixture_repo(tmp_path)
    out_file = tmp_path / 'scopes.txt'
    stdout, _, args = run_check(repo, out_file)
    assert args == [], args
    assert 'stale tasks' in stdout


def test_check_runs_everything_only_when_asked_for_full(tmp_path):
    repo = fixture_repo(tmp_path)
    out_file = tmp_path / 'scopes.txt'
    stdout, _, args = run_check(repo, out_file, R2S_FULL='1')
    assert args == ['--force'], args
    assert 'FULL' in stdout


def test_check_maps_each_e2e_backend_to_its_legs(tmp_path):
    repo = fixture_repo(tmp_path)
    out_file = tmp_path / 'scopes.txt'
    _, _, args = run_check(repo, out_file, E2E='posix')
    assert args == ['e2e-posix'], args
    _, _, args = run_check(repo, out_file, E2E='both')
    assert args == ['e2e-avr', 'e2e-posix'], args


def pass_vars():
    """The variables make forwards into the dev image, one per list line."""
    return (ROOT / 'scripts' / 'pass-vars.list').read_text().split()


def test_make_forwards_every_suite_scope():
    """A scope make accepts must reach the container, or it silently runs nothing."""
    text = (ROOT / 'scripts' / 'ci-test.sh').read_text()
    header = '\n'.join(
        line
        for line in text.splitlines()
        if 'Scope with' in line or re.match(r'^# [A-Z]{2,}', line)
    )
    scopes = set(re.findall(r'\b([A-Z][A-Z0-9]*)(?:=1|=<backend>)', header))
    for scope in sorted(scopes):
        assert scope in pass_vars(), f'make drops {scope} before the container'


def test_make_forwards_the_e2e_loop_narrowers():
    # tests/e2e/README.md documents both for a quicker local loop; make
    # must hand them into the dev image or they are silently ignored.
    for var in ('R2S_RMWS', 'R2S_E2E_BOARDS', 'R2S_E2E_BAUDS'):
        assert var in pass_vars(), f'make drops {var} before the container'


def test_make_forwards_the_suite_lock_wait():
    # scripts/ci-test.sh queues on the suite lock when R2S_SUITE_LOCK_WAIT=1;
    # make must hand it into the dev image or queueing never works.
    assert 'R2S_SUITE_LOCK_WAIT' in (ROOT / 'scripts' / 'ci-test.sh').read_text()
    assert 'R2S_SUITE_LOCK_WAIT' in pass_vars(), (
        'make drops R2S_SUITE_LOCK_WAIT before the container'
    )


def test_make_forwards_the_build_lock_nowait():
    # scripts/with-build-lock.sh refuses the build lock when
    # R2S_LOCK_NOWAIT=1; make must hand it into the dev image too.
    assert 'R2S_LOCK_NOWAIT' in (ROOT / 'scripts' / 'with-build-lock.sh').read_text()
    assert 'R2S_LOCK_NOWAIT' in pass_vars(), 'make drops R2S_LOCK_NOWAIT before the container'


def test_make_forwards_the_host_gate_knobs():
    # scripts/ci-test.sh caps concurrent suites with R2S_SUITE_SLOTS and
    # bounds the wait with R2S_SUITE_HOST_WAIT_SECS; make must hand both
    # into the dev image or the gate stays at its defaults.
    text = (ROOT / 'scripts' / 'ci-test.sh').read_text()
    for var in ('R2S_SUITE_SLOTS', 'R2S_SUITE_HOST_WAIT_SECS'):
        assert var in text, f'ci-test.sh never reads {var}'
        assert var in pass_vars(), f'make drops {var} before the container'
