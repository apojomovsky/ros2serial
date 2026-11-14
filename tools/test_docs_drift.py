# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The drift gate fails on each class of documentation drift it guards.

Each check reads the tree, so every case builds a throwaway tree with one
defect in it and asserts the checker names that defect. The last test runs
the gate against this repository, which fails while any drift stands.
"""

import subprocess
from pathlib import Path

import docs_drift

ROOT = Path(__file__).resolve().parent.parent


def _tree(tmp_path: Path, files: dict[str, str]) -> Path:
    """A git repository holding exactly these files."""
    repo = tmp_path / 'repo'
    repo.mkdir()
    subprocess.run(['git', 'init', '-q', '-b', 'master', str(repo)], check=True)
    subprocess.run(['git', 'config', 'user.email', 't@example.com'], cwd=repo, check=True)
    subprocess.run(['git', 'config', 'user.name', 't'], cwd=repo, check=True)
    for name, text in files.items():
        target = repo / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
    subprocess.run(['git', 'add', '-A'], cwd=repo, check=True)
    subprocess.run(['git', 'commit', '-q', '-m', 'test(docs): fixture'], cwd=repo, check=True)
    return repo


def _check(repo: Path, monkeypatch, check) -> list[str]:
    """Run one check against the throwaway tree."""
    monkeypatch.setattr(docs_drift, 'ROOT', repo)
    monkeypatch.setattr(docs_drift, 'USER_DOCS', tuple(_docs(repo)))
    return check()


def _docs(repo: Path) -> list[str]:
    """The user documents the throwaway tree actually holds."""
    return [path for path in docs_drift.USER_DOCS if (repo / path).is_file()]


_TWINLESS = ('plan', 'bridge-images', 'e2e-isolation', 'nightly')
_JOBS = ('lint',) + _TWINLESS + ('scripts-save',)
# One entry per top-level directory the fixture tracks, besides the
# directories git and the fixture tooling create.
_DIRS = ('.github', 'docs', 'gen', 'tests', 'examples')

BASE = {
    'Makefile': (
        'help:\n\t@true\n\ntest:\n\t@true\n\ncheck:\n\t@true\n\n'
        'lint-docs:\n\t@true\n\ntest-core:\n\t@true\n'
    ),
    '.github/workflows/ci.yml': (
        'jobs:\n' + ''.join(f'  {job}:\n    steps: []\n' for job in _JOBS)
    ),
    'gen/r2s_gen/schema/r2s.schema.json': (
        '{"type": "object", "properties": {"node": {"type": "string"}}}'
    ),
    'docs/00-design.md': (
        '## Repository layout\n\n```\nrepo/\n'
        + ''.join(f'├── {name}/\n' for name in _DIRS)
        + '```\n\n'
        '## CI on GitHub Actions\n\n| Workflow | Trigger | Jobs |\n| --- | --- | --- |\n'
        '| `ci.yml` | Push | ' + ', '.join(f'`{job}`' for job in _JOBS) + ' |\n\n'
        '### Config schema\n\n| Key | Type |\n| --- | --- |\n| `node` | string |\n\n'
        '### Type hash\n'
    ),
    'docs/01-testing.md': (
        '## 10. Key end-to-end scenarios\n\n1. A scenario.\n\n## 11. Tickets this adds\n'
    ),
    'tests/e2e/test_scenario_1_posix.py': '"""Scenario 1."""\n',
    'tests/e2e/run-avr.sh': 'for scenario in 1; do\n  true\ndone\n',
    'CONTRIBUTING.md': (
        '| Job | Command |\n| --- | --- |\n| `lint` | `make test` |\n'
        '| `plan` | `make check` |\n\n'
        + 'No local twin: '
        + ', '.join(f'`{job}`' for job in _TWINLESS)
        + '; the `-save` jobs persist a checksum after a green matrix.\n'
    ),
    'examples/README.md': '| Sketch |\n| --- |\n| `hello` |\n',
    'examples/arduino_hello/arduino_hello.ino': 'void setup() {}\n',
}


def _with(name: str, text: str) -> dict[str, str]:
    """The base tree with one file replaced or added."""
    files = dict(BASE)
    files[name] = text
    return files


def test_a_consistent_tree_passes(tmp_path, monkeypatch):
    repo = _tree(tmp_path, BASE)
    monkeypatch.setattr(docs_drift, 'ROOT', repo)
    monkeypatch.setattr(docs_drift, 'USER_DOCS', tuple(_docs(repo)))
    found = docs_drift.problems()
    assert found == [], found


def test_a_documented_target_the_makefile_lacks_is_named(tmp_path, monkeypatch):
    files = _with('README.md', '```sh\nmake lint-docs\nmake not-a-target\n```\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_make_targets)
    assert found == ['README.md: `make not-a-target` matches no Makefile target']


def test_a_make_command_in_prose_is_not_read_as_a_target(tmp_path, monkeypatch):
    repo = _tree(tmp_path, _with('README.md', 'A real board would make its own decision.\n'))
    assert _check(repo, monkeypatch, docs_drift.check_make_targets) == []


def test_a_ci_job_contributing_never_maps_is_named(tmp_path, monkeypatch):
    text = BASE['CONTRIBUTING.md'].replace('| `lint` | `make test` |\n', '')
    repo = _tree(tmp_path, _with('CONTRIBUTING.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_contributing_jobs)
    assert 'CONTRIBUTING.md: the lint job has no documented local target' in found


def test_a_contributing_row_naming_a_phantom_job_is_named(tmp_path, monkeypatch):
    text = BASE['CONTRIBUTING.md'].replace(
        '| `plan` | `make check` |', '| `plan` | `make check` |\n| `ghost` | `make test` |'
    )
    repo = _tree(tmp_path, _with('CONTRIBUTING.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_contributing_jobs)
    assert 'CONTRIBUTING.md: `ghost` is no ci.yml job' in found


def test_a_job_the_design_table_misses_is_named(tmp_path, monkeypatch):
    text = BASE['docs/00-design.md'].replace('`lint`, ', '')
    repo = _tree(tmp_path, _with('docs/00-design.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_design_jobs)
    assert found == ['docs/00-design.md: the CI table misses the lint job']


def test_a_scenario_file_no_entry_documents_is_named(tmp_path, monkeypatch):
    files = _with('tests/e2e/test_scenario_2_posix.py', '"""Scenario 2."""\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_scenarios)
    assert 'docs/01-testing.md: scenario 2 runs in CI but section 10 does not list it' in found


def test_a_documented_scenario_with_no_file_must_say_so(tmp_path, monkeypatch):
    text = BASE['docs/01-testing.md'].replace(
        '1. A scenario.\n', '1. A scenario.\n5. A bridge test.\n'
    )
    repo = _tree(tmp_path, _with('docs/01-testing.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_scenarios)
    assert 'docs/01-testing.md: scenario 5 has no test file and does not say so' in found


def test_the_no_e2e_marker_satisfies_the_scenario_rule(tmp_path, monkeypatch):
    text = BASE['docs/01-testing.md'].replace(
        '1. A scenario.\n', '1. A scenario.\n5. A bridge test; no end-to-end scenario runs it.\n'
    )
    repo = _tree(tmp_path, _with('docs/01-testing.md', text))
    assert _check(repo, monkeypatch, docs_drift.check_scenarios) == []


def test_an_avr_run_the_entry_list_omits_is_named(tmp_path, monkeypatch):
    loop = 'for scenario in 1 7; do\n  true\ndone\n'
    repo = _tree(tmp_path, _with('tests/e2e/run-avr.sh', loop))
    found = _check(repo, monkeypatch, docs_drift.check_scenarios)
    assert (
        'docs/01-testing.md: run-avr.sh runs scenario 7, which section 10 does not list' in found
    )


def test_a_tracked_directory_the_diagram_omits_is_named(tmp_path, monkeypatch):
    files = _with('benchmark/run.py', 'x\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_layout_diagram)
    assert 'docs/00-design.md: the layout diagram omits the benchmark/ directory' in found


def test_a_macro_no_source_carries_is_named(tmp_path, monkeypatch):
    repo = _tree(tmp_path, _with('README.md', 'Built with `R2S_WITH_GHOST`.\n'))
    found = _check(repo, monkeypatch, docs_drift.check_macros)
    assert found == ['README.md: `R2S_WITH_GHOST` is in no source file']


def test_a_macro_a_source_defines_passes(tmp_path, monkeypatch):
    files = _with('README.md', 'Built with `R2S_WITH_GHOST`.\n')
    files['core/a.c'] = '#ifdef R2S_WITH_GHOST\n#endif\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_macros) == []


def test_a_schema_key_the_config_table_omits_is_named(tmp_path, monkeypatch):
    schema = '{"type": "object", "properties": {"node": {}, "topics": {}}}'
    repo = _tree(tmp_path, _with('gen/r2s_gen/schema/r2s.schema.json', schema))
    found = _check(repo, monkeypatch, docs_drift.check_config_table)
    assert found == ['docs/00-design.md: r2s.yaml accepts `topics`, which the config table omits']


def test_a_config_row_the_schema_rejects_is_named(tmp_path, monkeypatch):
    text = BASE['docs/00-design.md'].replace(
        '| `node` | string |', '| `node` | string |\n| `phantom` | string |'
    )
    repo = _tree(tmp_path, _with('docs/00-design.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_config_table)
    assert found == [
        'docs/00-design.md: the config table documents `phantom`, which r2s.yaml rejects'
    ]


def test_an_example_the_readme_never_names_is_named(tmp_path, monkeypatch):
    files = _with('examples/arduino_ghost/arduino_ghost.ino', 'void setup() {}\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_examples)
    assert found == ['examples/README.md: the arduino_ghost example is not listed']


def test_the_real_tree_is_green():
    """Every drift the ticket found is fixed, so the gate closes on this tree."""
    proc = subprocess.run(
        ['python3', str(ROOT / 'tools' / 'docs_drift.py')],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
