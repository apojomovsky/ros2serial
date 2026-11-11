# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The pre-push changed-files format gate, on throwaway repositories.

A push that adds a line past the width limit fails before the suite runs;
a clean change passes; a host without the tools skips with one line per
tool and still pushes.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(['git', *args], cwd=repo, capture_output=True, text=True, check=True)


def run(args, cwd, env_extra=None, stdin=None):
    env = {**os.environ, **(env_extra or {})}
    return subprocess.run(args, cwd=cwd, input=stdin, capture_output=True, text=True, env=env)


def commit(repo: Path, files: dict[str, str], message: str) -> None:
    for name, text in files.items():
        (repo / name).parent.mkdir(parents=True, exist_ok=True)
        (repo / name).write_text(text)
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', message)


def fixture(tmp_path: Path) -> tuple[Path, Path]:
    """A bare origin, a main clone on master, and a worktree on a branch."""
    origin = tmp_path / 'origin.git'
    git(tmp_path, 'init', '-q', '--bare', '-b', 'master', str(origin))
    main = tmp_path / 'main'
    main.mkdir()
    git(main, 'init', '-q', '-b', 'master')
    git(main, 'config', 'user.email', 't@example.com')
    git(main, 'config', 'user.name', 't')
    git(main, 'remote', 'add', 'origin', str(origin))
    commit(main, {'README.md': '# x\n'}, 'chore: start')
    git(main, 'push', '-q', 'origin', 'master')
    git(main, 'worktree', 'add', '-q', '.worktrees/feat', '-b', 'feat/1-thing')
    return main, main / '.worktrees' / 'feat'


def pre_push(wt: Path, line: str, **env) -> subprocess.CompletedProcess:
    return run(
        ['bash', str(ROOT / '.githooks/pre-push'), 'origin', 'url'],
        wt,
        {'R2S_PRE_PUSH_SUITE': 'true', **env},
        stdin=line,
    )


def new_branch_line(wt: Path) -> str:
    head = git(wt, 'rev-parse', 'HEAD').stdout.strip()
    return f'refs/heads/feat/1-thing {head} refs/heads/feat/1-thing {"0" * 40}\n'


def has_cpplint() -> bool:
    return shutil.which('ament_cpplint') is not None or (shutil.which('cpplint') is not None)


def ruff_at_pin() -> bool:
    try:
        pin = next(
            line.split('=', 1)[1].strip()
            for line in (ROOT / 'ci/versions.env').read_text().splitlines()
            if line.startswith('RUFF_VERSION=')
        )
    except StopIteration:
        return False
    proc = subprocess.run(
        ['python3', '-m', 'ruff', '--version'],
        capture_output=True,
        text=True,
    )
    return proc.returncode == 0 and proc.stdout.strip() == f'ruff {pin}'


def require_cpplint() -> None:
    if not has_cpplint():
        pytest.skip('no ament_cpplint or cpplint on PATH')


def require_ruff() -> None:
    if not ruff_at_pin():
        pytest.skip('no pinned ruff on PATH')


def stage_python_config(wt: Path) -> None:
    """Ruff reads its limits from the worktree, so stage the real files."""
    (wt / 'ci').mkdir(exist_ok=True)
    shutil.copy(ROOT / 'pyproject.toml', wt / 'pyproject.toml')
    shutil.copy(ROOT / 'ci/versions.env', wt / 'ci/versions.env')


def test_a_101_column_cpp_line_fails_the_push(tmp_path):
    """The added line is one past the cpplint limit CI enforces."""
    require_cpplint()
    _, wt = fixture(tmp_path)
    long_line = 'int ' + 'v' * 92 + ' = 0;'
    assert len(long_line) == 101, len(long_line)
    commit(
        wt,
        {'bridge/r2s_bridge/src/overflow.cpp': 'int overflow_flag = 0;\n' + long_line + '\n'},
        'feat(bridge): stage the relay',
    )
    result = pre_push(wt, new_branch_line(wt))
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert 'overflow.cpp:2:' in out, out


def test_a_100_column_python_line_fails_the_push(tmp_path):
    """The added line is one past the ruff limit CI enforces."""
    require_ruff()
    _, wt = fixture(tmp_path)
    stage_python_config(wt)
    long_line = ("x = '" + 'y' * 100)[:99] + "'"
    assert len(long_line) == 100, len(long_line)
    commit(wt, {'gen/r2s_gen/wide.py': long_line + '\n'}, 'feat(gen): stage a table')
    result = pre_push(wt, new_branch_line(wt))
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert 'wide.py:1:' in out, out


def test_a_clean_change_passes_the_push(tmp_path):
    """Lines exactly at each limit are accepted, so the gate never over-blocks."""
    require_cpplint()
    require_ruff()
    _, wt = fixture(tmp_path)
    stage_python_config(wt)
    cpp_line = ('static const char *msg = "' + 'z' * 100)[:98] + '";'
    assert len(cpp_line) == 100, len(cpp_line)
    py_line = ("x = '" + 'y' * 100)[:98] + "'"
    assert len(py_line) == 99, len(py_line)
    commit(
        wt,
        {
            'bridge/r2s_bridge/src/tidy.cpp': 'int tidy_flag = 0;\n' + cpp_line + '\n',
            'gen/r2s_gen/tidy.py': py_line + '\n',
        },
        'feat(bridge): stage a tidy table',
    )
    result = pre_push(wt, new_branch_line(wt))
    out = result.stdout + result.stderr
    assert result.returncode == 0, out


def test_a_host_without_the_tools_skips_and_still_pushes(tmp_path):
    """One skip line per missing tool, and the push goes through."""
    _, wt = fixture(tmp_path)
    commit(
        wt,
        {
            'bridge/r2s_bridge/src/overflow.cpp': 'int overflow_flag = 0;\n',
            'gen/r2s_gen/wide.py': "x = 'y'\n",
        },
        'feat(bridge): stage the relay',
    )
    bare = tmp_path / 'bare-bin'
    bare.mkdir()
    for name in ('git', 'awk', 'grep', 'sed'):
        found = shutil.which(name)
        assert found is not None, name
        os.symlink(found, bare / name)
    env = {**os.environ, 'PATH': str(bare)}
    result = subprocess.run(
        [str(shutil.which('bash')), str(ROOT / 'scripts/changed-format.sh')],
        cwd=wt,
        input=new_branch_line(wt),
        capture_output=True,
        text=True,
        env=env,
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert out.count('SKIP') == 3, out
    for tool in ('ament_cpplint', 'ruff', 'uncrustify'):
        assert tool in out, out


def test_the_hook_and_ci_agree_on_the_limits():
    """The enforced widths equal the configs CI runs with."""
    text = (ROOT / 'pyproject.toml').read_text()
    assert '\nline-length = 99\n' in text, text
    cfg = (ROOT / 'tools/vendor/ament_code_style.cfg').read_text()
    assert re.search(r'^code_width\s*=\s*100\b', cfg, re.M) is not None
