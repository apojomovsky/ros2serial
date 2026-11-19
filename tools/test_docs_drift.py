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
import pytest

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
    return check()


def _docs(repo: Path) -> list[str]:
    """The user documents the throwaway tree actually holds."""
    return [path for path in docs_drift.user_docs() if (repo / path).is_file()]


_TWINLESS = ('plan', 'bridge-images', 'e2e-isolation', 'nightly')
_JOBS = ('lint',) + _TWINLESS + ('scripts-save',)
# One entry per top-level directory the fixture tracks, besides the
# directories git and the fixture tooling create.
_DIRS = ('.github', 'docs', 'gen', 'tests', 'examples')

BASE = {
    'Makefile': (
        'help:\n\t@true\n\ntest:\n\t@true\n\ncheck:\n\t@true\n\n'
        'lint-docs:\n\t@true\n\ntest-core:\n\t@true\n\n'
        'compile:\n\t@$(call pending,compile,G10 in docs/00-design.md)\n'
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
        '### Type hash\n\n'
        'The strategy is [the testing document](01-testing.md).\n'
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
        + 'See [the design](docs/00-design.md) and [the examples](examples/README.md).\n'
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
    found = docs_drift.problems()
    assert found == [], found


def test_a_documented_target_the_makefile_lacks_is_named(tmp_path, monkeypatch):
    files = _with('README.md', '```sh\nmake lint-docs\nmake not-a-target\n```\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_make_targets)
    assert found == ['README.md: `make not-a-target` matches no Makefile target']


def test_a_target_the_makefile_implements_read_as_unbuilt_is_named(tmp_path, monkeypatch):
    files = _with('README.md', '`make test` is not built yet.\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_unbuilt_claims)
    assert found == ['README.md: `make test` is called not built, but the Makefile implements it']


def test_a_pending_target_read_as_unbuilt_passes(tmp_path, monkeypatch):
    files = _with('README.md', '`make compile` is not built yet.\n')
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_unbuilt_claims) == []


def test_an_unbuilt_claim_naming_no_target_is_not_judged(tmp_path, monkeypatch):
    """The phrase alone names nothing the Makefile can confirm, so it is not read."""
    files = _with('README.md', 'A one-command board check is not built yet.\n')
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_unbuilt_claims) == []


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


def test_a_missing_root_entry_is_not_satisfied_by_a_nested_path(tmp_path, monkeypatch):
    """`tools/vectors_check.py` on another row is not the `tools/` entry."""
    text = BASE['docs/00-design.md'].replace(
        '├── gen/', '│   └── schema  x (tools/vectors_check.py)\n├── gen/'
    )
    text = ''.join(
        line for line in text.splitlines(keepends=True) if not line.startswith('├── tests/')
    )
    repo = _tree(tmp_path, _with('docs/00-design.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_layout_diagram)
    assert 'docs/00-design.md: the layout diagram omits the tests/ directory' in found


def test_a_directory_the_diagram_invents_is_named(tmp_path, monkeypatch):
    # The entry that got through carried no trailing slash, so the fixture
    # carries none either.
    text = BASE['docs/00-design.md'].replace('├── docs/', '├── ghostdir  invented\n├── docs/')
    repo = _tree(tmp_path, _with('docs/00-design.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_layout_diagram)
    assert (
        'docs/00-design.md: the layout diagram names the ghostdir/ directory, which the tree lacks'
        in found
    )


def test_a_diagram_entry_that_is_a_tracked_file_is_not_a_directory(tmp_path, monkeypatch):
    text = BASE['docs/00-design.md'].replace(
        '├── docs/', '├── README.md  the walkthrough\n├── docs/'
    )
    files = _with('docs/00-design.md', text)
    files['README.md'] = '# x\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_layout_diagram) == []


def test_a_macro_the_spec_defines_counts_as_defined(tmp_path, monkeypatch):
    """DSC-24 defines a macro the C sources never mention."""
    spec = '| Kinds 7 and 8 occupy 8 bytes with and without `R2S_NO_INT64` |\n'
    repo = _tree(tmp_path, _with('spec/protocol.md', spec))
    monkeypatch.setattr(docs_drift, 'ROOT', repo)
    monkeypatch.setattr(docs_drift, 'user_docs', lambda: ('README.md',))
    (repo / 'README.md').write_text('Built with `R2S_NO_INT64`.\n', encoding='utf-8')
    subprocess.run(['git', 'add', '-A'], cwd=repo, check=True)
    subprocess.run(['git', 'commit', '-q', '-m', 'x'], cwd=repo, check=True)
    assert docs_drift.check_macros() == []


def test_a_macro_mentioned_only_in_prose_fails(tmp_path, monkeypatch):
    """A changelog sentence naming a macro must not stand in for a definition."""
    files = _with('README.md', 'Built with `R2S_WITH_GHOST`.\n')
    files['CHANGELOG.md'] = 'Mentions `R2S_WITH_GHOST` in passing.\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_macros)
    assert found == ['README.md: `R2S_WITH_GHOST` is in no source file']


def test_a_new_page_under_docs_is_read(tmp_path, monkeypatch):
    files = _with('docs/06-new-doc.md', 'Run `make not-a-target`.\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_make_targets)
    assert found == ['docs/06-new-doc.md: `make not-a-target` matches no Makefile target']


def test_a_twinless_job_row_without_its_reason_fails(tmp_path, monkeypatch):
    """The row alone is no excuse: the reason lives in the prose."""
    text = BASE['CONTRIBUTING.md'].replace('`e2e-isolation`', 'e2e-isolation')
    repo = _tree(tmp_path, _with('CONTRIBUTING.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_contributing_jobs)
    assert any(
        '`e2e-isolation` twinless exemption lost its reason' in problem for problem in found
    )


def test_an_example_named_nowhere_is_named(tmp_path, monkeypatch):
    text = BASE['examples/README.md'].replace('| `hello` |\n', '')
    repo = _tree(tmp_path, _with('examples/README.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_examples)
    assert found == ['examples/README.md: the arduino_hello example is not listed']


def test_a_phantom_job_the_design_table_names_is_named(tmp_path, monkeypatch):
    text = BASE['docs/00-design.md'].replace('| Push | `lint`', '| Push | `ghost`, `lint`')
    repo = _tree(tmp_path, _with('docs/00-design.md', text))
    found = _check(repo, monkeypatch, docs_drift.check_design_jobs)
    assert 'docs/00-design.md: the CI table names `ghost`, which is no ci.yml job' in found


BOUNDS_PAGE = 'docs/message-bounds.md'
BOUNDS_HEADINGS = '# Message bounds\n\n## Where a bound comes from\n'


def test_a_page_that_mentions_bounds_and_links_the_section_passes(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/bounds-user.md'] = (
        'Bound the field with `string<=12`. See [message bounds](message-bounds.md).\n'
    )
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


def test_a_page_that_mentions_bounds_without_the_link_is_named(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/bounds-user.md'] = 'Bound the field with `string<=12`.\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == [
        'docs/bounds-user.md: mentions bounds but does not link docs/message-bounds.md'
    ]


def test_a_link_to_a_heading_the_bounds_page_lacks_is_named(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/bounds-user.md'] = (
        'Bound the field with `string<=12`; see [here](message-bounds.md#nowhere).\n'
    )
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == [
        'docs/bounds-user.md: link to message-bounds.md#nowhere matches no heading'
        ' on the bounds page'
    ]


def test_a_link_to_a_missing_bounds_page_is_named(tmp_path, monkeypatch):
    files = _with('docs/bounds-user.md', 'Bound it with `string<=12`; [see](message-bounds.md).\n')
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == ['docs/bounds-user.md: link to message-bounds.md names no page']


def test_the_bounds_page_does_not_have_to_link_itself(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS + 'Bound it with `string<=12`.\n')
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


def test_a_root_page_links_the_bounds_page_by_its_docs_path(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['README.md'] = 'Bound it with `string<=12`; see [bounds](docs/message-bounds.md).\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


def test_a_published_pointer_to_a_page_the_site_lacks_is_named(tmp_path, monkeypatch):
    """The generator's messages carry a site URL, so a dropped page leaves a dead pointer."""
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['gen/r2s_gen/docs.py'] = (
        "BOUNDS_URL = 'https://apojomovsky.github.io/ros2serial/nowhere.html'\n"
    )
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_docs_urls)
    assert found == ['gen/r2s_gen/docs.py: nowhere.html is no page the site publishes']


def test_a_published_pointer_to_a_page_the_site_carries_passes(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['tools/site_build.py'] = (
        'from collections import namedtuple\n'
        "Page = namedtuple('Page', 'source output title blurb')\n"
        "PAGES = (Page('docs/message-bounds.md', 'message-bounds.html', 'Bounds', 'x'),)\n"
    )
    files['gen/r2s_gen/docs.py'] = (
        "BOUNDS_URL = 'https://apojomovsky.github.io/ros2serial/message-bounds.html'\n"
    )
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_docs_urls) == []


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


def test_a_page_under_a_subdirectory_links_the_section_by_a_relative_path(tmp_path, monkeypatch):
    """docs/research/x.md reaches the page as ../message-bounds.md, not as a sibling."""
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/research/x.md'] = 'Bound with `string<=12`; [here](../message-bounds.md).\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


def test_a_nested_page_whose_link_does_not_reach_the_section_is_named(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/research/x.md'] = 'Bound with `string<=12`; [here](message-bounds.md).\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == [
        'docs/research/x.md: mentions bounds but does not link docs/message-bounds.md'
    ]


def test_a_dead_anchor_from_a_nested_page_is_named(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/research/x.md'] = 'Bound with `string<=12`; [here](../message-bounds.md#gone).\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == [
        'docs/research/x.md: link to ../message-bounds.md#gone matches no heading'
        ' on the bounds page'
    ]


def test_a_bound_in_ordinary_prose_counts_as_mentioning_bounds(tmp_path, monkeypatch):
    """The gate cannot be satisfied by avoiding the token set: plain prose counts."""
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/x.md'] = 'Publishing past the bound fails instead of sending.\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_bounds_links)
    assert found == ['docs/x.md: mentions bounds but does not link docs/message-bounds.md']


def test_a_timing_bound_is_not_a_message_bound(tmp_path, monkeypatch):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/x.md'] = 'The sync scenario judges stamps within a timing bound.\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


@pytest.mark.parametrize(
    'sentence',
    [
        'The sequence bound of JointState.position is six; raise it in r2s.yaml.',
        'Bounds come from the .msg definition or an r2s.yaml entry.',
        'Bound each field in its .msg when you can.',
        'Raise the upper bound on JointState.name so longer names fit.',
    ],
)
def test_a_message_bound_in_any_spelling_counts(sentence):
    """The gate reads the sentences a reader writes about storage, not a fixed list."""
    assert docs_drift.states_a_bound(sentence)


@pytest.mark.parametrize(
    'sentence',
    [
        'No crash, hang or out-of-bounds access on any input.',
        'The decoder does a bounds check before it copies.',
        'The sync scenario judges stamps within a timing bound.',
        'The stack budget holds; the analysis above proves the bound.',
        'The loader rejects only what its padding-blind lower bound proves.',
    ],
)
def test_a_limit_that_is_not_a_message_bound_does_not_count(sentence):
    assert not docs_drift.states_a_bound(sentence)


@pytest.mark.parametrize(
    'link',
    [
        '[s](message-bounds.md "bounds")',
        "[s](message-bounds.md 'bounds')",
        '[s](<message-bounds.md>)',
        '[s](message-bounds.md (bounds))',
    ],
)
def test_a_link_with_a_title_or_angle_brackets_still_reaches_the_section(
    tmp_path, monkeypatch, link
):
    files = _with('docs/message-bounds.md', BOUNDS_HEADINGS)
    files['docs/x.md'] = f'Bound with `string<=12`; see {link}.\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_bounds_links) == []


DOCS_BASE = dict(BASE)


def test_a_page_no_reader_page_reaches_is_named(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['docs/orphan.md'] = 'A page nothing links.\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_pages_reachable)
    assert found == ['docs/orphan.md: no page a reader starts from reaches this one']


def test_a_page_reached_through_another_page_passes(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['docs/00-design.md'] = BASE['docs/00-design.md'] + ('\nSee [the orphan](orphan.md).\n')
    files['docs/orphan.md'] = 'A page the design reaches.\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_pages_reachable) == []


def test_a_link_that_leaves_the_tree_is_not_an_edge(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['docs/00-design.md'] = BASE['docs/00-design.md'] + (
        '\nSee [upstream](https://example.com/x.md).\n'
    )
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_pages_reachable) == []


def test_a_subcommand_the_generator_lacks_is_named(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['gen/r2s_gen/cli.py'] = (
        'import argparse\n\n\ndef build():\n    parser = argparse.ArgumentParser()\n'
        '    subparsers = parser.add_subparsers()\n'
        "    subparsers.add_parser('version', help='x')\n"
    )
    files['README.md'] = (
        '```sh\nros2 run r2s_gen r2s-gen version\nr2s-gen frobnicate --out x\n```\n'
    )
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_generator_commands)
    assert found == ['README.md: `r2s-gen frobnicate` is no generator subcommand']


def test_a_documented_subcommand_the_parser_declares_passes(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['gen/r2s_gen/cli.py'] = (
        'import argparse\n\n\ndef build():\n    parser = argparse.ArgumentParser()\n'
        '    subparsers = parser.add_subparsers()\n'
        "    subparsers.add_parser('version', help='x')\n"
    )
    files['README.md'] = '```sh\nros2 run r2s_gen r2s-gen version\n```\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_generator_commands) == []


def test_a_ros2_run_pair_the_packages_lack_is_named(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['gen/setup.py'] = (
        'from setuptools import setup\n\nsetup(\n    entry_points={\n'
        "        'console_scripts': ['r2s-gen = r2s_gen.cli:main'],\n    },\n)\n"
    )
    files['gen/package.xml'] = '<package><name>r2s_gen</name></package>\n'
    files['README.md'] = '```sh\nros2 run r2s_gen r2s-ghost version\n```\n'
    repo = _tree(tmp_path, files)
    found = _check(repo, monkeypatch, docs_drift.check_ros2_run)
    assert found == ['README.md: `ros2 run r2s_gen r2s-ghost` names no installed executable']


def test_a_ros2_run_pair_the_packages_install_passes(tmp_path, monkeypatch):
    files = dict(DOCS_BASE)
    files['gen/setup.py'] = (
        'from setuptools import setup\n\nsetup(\n    entry_points={\n'
        "        'console_scripts': ['r2s-gen = r2s_gen.cli:main'],\n    },\n)\n"
    )
    files['gen/package.xml'] = '<package><name>r2s_gen</name></package>\n'
    files['README.md'] = '```sh\nros2 run r2s_gen r2s-gen version\n```\n'
    repo = _tree(tmp_path, files)
    assert _check(repo, monkeypatch, docs_drift.check_ros2_run) == []
