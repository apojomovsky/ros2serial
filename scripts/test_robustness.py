# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The robustness numbers live in one place and the docs repeat them.

tools/robustness.yaml is the single source for the nightly mutation
floors and the fuzz target map; docs/robustness.md publishes the method
and the current thresholds beside the budgets page. These tests pin that
contract: the yaml carries the section 6 floors, every target maps to a
real seed corpus the fuzz script runs, and the docs page repeats every
floor and every target instead of drifting from the file.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
ROBUSTNESS = ROOT / 'tools' / 'robustness.yaml'
DOCS = ROOT / 'docs' / 'robustness.md'

SECTION_6_FLOORS = {'core': 85, 'cpp': 75, 'gen': 80, 'bridge': 70}


def robustness():
    assert ROBUSTNESS.is_file(), 'tools/robustness.yaml is missing'
    return yaml.safe_load(ROBUSTNESS.read_text(encoding='utf-8'))


def test_the_yaml_carries_the_section_6_mutation_floors():
    assert robustness()['mutation_floors'] == SECTION_6_FLOORS


def test_every_fuzz_target_has_a_seed_corpus_the_fuzz_script_runs():
    fuzz_script = (ROOT / 'scripts' / 'fuzz.sh').read_text(encoding='utf-8')
    for target in robustness()['fuzz_targets']:
        corpus = ROOT / 'core' / 'fuzz' / 'corpus' / target
        assert corpus.is_dir(), f'core/fuzz/corpus/{target} is missing'
        assert target in fuzz_script, f'{target} is not in scripts/fuzz.sh'


def test_every_mapped_source_file_exists():
    for target, sources in robustness()['fuzz_targets'].items():
        for source in sources:
            assert (ROOT / source).is_file(), f'{target} maps a missing file: {source}'


def test_the_docs_page_exists_and_repeats_every_floor():
    assert DOCS.is_file(), 'docs/robustness.md is missing'
    text = DOCS.read_text(encoding='utf-8')
    for component, floor in SECTION_6_FLOORS.items():
        assert component in text, f'{component} is not in docs/robustness.md'
        assert f'{floor}%' in text, f'{floor}% is not in docs/robustness.md'


def test_the_docs_page_names_every_fuzz_target():
    text = DOCS.read_text(encoding='utf-8')
    for target in robustness()['fuzz_targets']:
        assert target in text, f'{target} is not in docs/robustness.md'


def test_the_docs_page_is_published_on_the_site():
    import tools.site_build as site_build

    sources = [page.source for page in site_build.PAGES]
    assert 'docs/robustness.md' in sources
