# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""A pull request runs only the jobs its files can affect.

Each row names changed files, the jobs the map must select and the jobs it
must leave out. The trees every job builds in select the full matrix, and so
does a path no section reads: an unknown path must never select too few.
"""

import subprocess
from pathlib import Path

import ci_jobs
import yaml

ROOT = Path(__file__).resolve().parent.parent

# Changed files, the jobs that must run, and the jobs that must not.
CASES = (
    (
        ['docs/design.md'],
        {'lint', 'docs', 'mermaid'},
        {'scripts', 'core', 'bridge', 'readme', 'budgets', 'e2e-avr', 'e2e-posix'},
    ),
    (
        # The walkthrough check greps the README for its anchors, so a README
        # edit can break it; the other prose gates run as well.
        ['README.md'],
        {'lint', 'docs', 'mermaid', 'readme'},
        {'scripts', 'bridge', 'core', 'budgets', 'e2e-avr'},
    ),
    (
        ['CHANGELOG.md'],
        {'lint', 'docs', 'mermaid'},
        {'scripts', 'readme', 'bridge', 'core'},
    ),
    (
        ['tools/site_build.py'],
        {'lint', 'scripts', 'docs'},
        {'bridge', 'core', 'e2e-avr', 'e2e-posix', 'budgets'},
    ),
    (
        ['tools/capacity_report.py'],
        {'lint', 'scripts', 'budgets'},
        {'bridge', 'core', 'docs', 'e2e-avr', 'readme'},
    ),
    (
        ['tools/fuzz_corpus.py'],
        {'lint', 'scripts', 'fuzz-smoke'},
        {'bridge', 'core', 'budgets', 'docs'},
    ),
    (
        ['tools/coverage_gate.py'],
        {'lint', 'scripts', 'coverage'},
        {'bridge', 'core', 'docs', 'budgets'},
    ),
    (
        ['tools/zone_check.py'],
        {'lint', 'scripts'},
        {'bridge', 'core', 'docs', 'budgets', 'fuzz-smoke'},
    ),
    (
        ['scripts/build-examples-esp32.sh'],
        {'lint', 'scripts', 'esp32', 'budgets'},
        {'bridge', 'core', 'docs', 'platformio'},
    ),
    (
        ['spec/vectors/frame.yaml'],
        {'lint', 'scripts', 'core', 'gen', 'bridge', 'fuzz-smoke'},
        {'docs', 'e2e-avr', 'e2e-posix', 'budgets'},
    ),
    (
        ['tests/e2e/test_isolation.py'],
        {'lint', 'scripts', 'e2e-isolation'},
        {'e2e-avr', 'e2e-posix', 'bridge'},
    ),
    (
        ['tests/harness/simavr/run.sh'],
        {'lint', 'readme', 'e2e-avr'},
        {'bridge', 'core', 'e2e-posix', 'scripts'},
    ),
    (
        ['examples/arduino_minimal/arduino_minimal.ino'],
        {'lint', 'build-matrix', 'docs', 'esp32', 'gen', 'readme', 'budgets'},
        {'bridge', 'e2e-posix', 'core'},
    ),
)


def selected(paths):
    return ci_jobs.jobs_for(ci_jobs.select(paths))


def test_the_table_selects_what_the_gates_read():
    for paths, must, must_not in CASES:
        chosen = selected(paths)
        assert must <= chosen, f'{paths} misses {sorted(must - chosen)}'
        assert not (must_not & chosen), f'{paths} must not run {sorted(must_not & chosen)}'


def test_a_shared_tree_runs_the_full_matrix():
    """core, cpp, ports, bridge, gen and the image build into every job."""
    shared = (
        'core/src/framer.c',
        'cpp/include/r2s/r2s.hpp',
        'ports/stm32/build.sh',
        'bridge/src/session.cpp',
        'gen/r2s_gen/cli.py',
        '.github/workflows/ci.yml',
        '.github/actions/dev-image/action.yml',
        'ci/versions.env',
        'Dockerfile',
        '.dockerignore',
        'Makefile',
        'Taskfile.yml',
        'VERSION',
        'conftest.py',
        'pyproject.toml',
    )
    for path in shared:
        assert ci_jobs.select([path]) == {ci_jobs.FULL: '1'}, path


def test_the_map_itself_runs_the_full_matrix():
    assert ci_jobs.select(['tools/ci_jobs.py']) == {ci_jobs.FULL: '1'}


def test_a_path_no_section_reads_runs_the_full_matrix():
    assert ci_jobs.select(['notes/todo.txt']) == {ci_jobs.FULL: '1'}


def test_an_empty_diff_never_reads_as_green():
    assert ci_jobs.select([]) == {ci_jobs.FULL: '1'}


def test_the_standing_gate_runs_whatever_changed():
    for paths, _, _ in CASES:
        assert 'lint' in selected(paths), paths


def test_every_selection_names_a_section_job():
    """The jobs the map names are the jobs ci.yml defines."""
    workflow = yaml.safe_load(
        (ROOT / '.github' / 'workflows' / 'ci.yml').read_text(encoding='utf-8')
    )['jobs']
    defined = {
        name
        for name in workflow
        if name not in ('plan', 'bridge-images')
        and not name.endswith('-save')
        # The nightly caller is label-gated, not scope-gated.
        and name != 'nightly'
    }
    assert set(ci_jobs.JOBS) == defined
    selection = ci_jobs.select(['docs/design.md', 'tools/site_build.py'])
    assert set(selection) <= set(ci_jobs.OUTPUTS) | {ci_jobs.FULL}


def test_outputs_avoid_dashes_github_expression_syntax_reserves():
    for job, output in ci_jobs.JOBS.items():
        assert '-' not in output, f'{job} maps to {output}'


def git(repo, *args):
    return subprocess.run(['git', *args], cwd=repo, capture_output=True, text=True, check=True)


def head_commit(repo, message):
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', message)


def test_changed_paths_reads_the_merge_base_not_the_base_tip(tmp_path):
    repo = tmp_path / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'master')
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'docs').mkdir()
    (repo / 'docs' / 'page.md').write_text('one\n')
    head_commit(repo, 'docs: one')
    git(repo, 'branch', 'work')
    git(repo, 'checkout', '-q', 'work')
    (repo / 'README.md').write_text('probe\n')
    head_commit(repo, 'docs: readme')
    git(repo, 'checkout', '-q', 'master')
    (repo / 'docs' / 'page.md').write_text('two\n')
    head_commit(repo, 'docs: two')
    git(repo, 'checkout', '-q', 'work')
    assert ci_jobs.changed_paths('master', repo=repo) == ['README.md']


def test_changed_paths_names_both_sides_of_a_rename(tmp_path):
    repo = tmp_path / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'master')
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'core').mkdir()
    (repo / 'core' / 'framer.c').write_text('/* core */\n')
    head_commit(repo, 'feat(core): framer')
    git(repo, 'mv', 'core/framer.c', 'core/framing.c')
    head_commit(repo, 'refactor(core): rename')
    paths = ci_jobs.changed_paths('HEAD~1', repo=repo)
    assert 'core/framer.c' in paths, 'the old path is a file the change removes'
    assert 'core/framing.c' in paths


def test_a_base_that_cannot_be_read_falls_back_to_the_full_matrix(tmp_path, capsys):
    repo = tmp_path / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'master')
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'docs').mkdir()
    (repo / 'docs' / 'page.md').write_text('one\n')
    head_commit(repo, 'docs: one')
    code = ci_jobs.main(['--base', 'no-such-branch'], repo=repo)
    out = capsys.readouterr()
    assert code == 0
    assert out.out.splitlines() == ['full=1']
    assert 'no-such-branch' in out.err


def test_main_prints_one_line_per_selected_job(tmp_path, capsys):
    repo = tmp_path / 'repo'
    repo.mkdir()
    git(repo, 'init', '-q', '-b', 'master')
    git(repo, 'config', 'user.email', 't@example.com')
    git(repo, 'config', 'user.name', 't')
    (repo / 'README.md').write_text('one\n')
    head_commit(repo, 'docs: readme')
    git(repo, 'checkout', '-q', '-b', 'work')
    (repo / 'docs').mkdir()
    (repo / 'docs' / 'page.md').write_text('page\n')
    head_commit(repo, 'docs: page')
    code = ci_jobs.main(['--base', 'master'], repo=repo)
    lines = capsys.readouterr().out.split()
    assert code == 0
    assert lines == ['docs=1', 'lint=1', 'mermaid=1']
