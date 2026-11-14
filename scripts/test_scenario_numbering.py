# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""A scenario number names one scenario on every backend.

The backends share the numbering: `test_scenario_N_posix.py` and
`test_scenario_N_avr.py` drive the same behaviour, so a number carries the
same `covers` rule families on each backend that runs it. A slip that gives
one backend a different scenario under the same number fails here, and so
does a run-avr.sh loop entry with no avr file behind it.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / 'tests' / 'e2e'
RUN_AVR_SH = E2E / 'run-avr.sh'
SCENARIO_FILE = re.compile(r'^test_scenario_(\d+)_(\w+)\.py$')
COVERS = re.compile(r'^# covers: (.*)$', re.MULTILINE)
FAMILY = re.compile(r'^([A-Z]+)-\d+$')


def scenario_files():
    """The scenario number and backend of every scenario file, as a list of pairs."""
    found = []
    for path in sorted(E2E.glob('test_scenario_*_*.py')):
        match = SCENARIO_FILE.match(path.name)
        if match:
            found.append((match.group(1), match.group(2), path))
    return found


def families(path):
    """The rule families a scenario file claims, empty when it carries no marker."""
    match = COVERS.search(path.read_text(encoding='utf-8'))
    if not match:
        return set()
    return {
        FAMILY.match(rule.strip()).group(1)
        for rule in match.group(1).split(',')
        if FAMILY.match(rule.strip())
    }


def backends_by_number():
    """The backends each scenario number runs on, in file order."""
    by_number = {}
    for number, backend, path in scenario_files():
        by_number.setdefault(number, {})[backend] = path
    return by_number


def avr_loop_scenarios():
    """The scenario numbers run-avr.sh runs, read from its loop."""
    text = RUN_AVR_SH.read_text(encoding='utf-8')
    match = re.search(r'for scenario in ([0-9 ]+);', text)
    assert match, 'tests/e2e/run-avr.sh names no scenario loop'
    return match.group(1).split()


def test_a_number_keeps_its_rule_families_across_backends():
    collisions = []
    for number, paths in sorted(backends_by_number().items(), key=lambda item: int(item[0])):
        claimed = {backend: families(path) for backend, path in paths.items()}
        claimed = {backend: found for backend, found in claimed.items() if found}
        if len(claimed) < 2:
            continue
        shared = set.intersection(*claimed.values())
        if not shared:
            detail = ', '.join(
                f'{backend}: {sorted(found)}' for backend, found in sorted(claimed.items())
            )
            collisions.append(f'scenario {number} runs different scenarios per backend ({detail})')
    assert not collisions, '\n'.join(collisions)


def test_every_avr_scenario_in_the_loop_has_a_file():
    missing = [
        f'run-avr.sh runs scenario {number}, which has no test_scenario_{number}_avr.py'
        for number in avr_loop_scenarios()
        if not (E2E / f'test_scenario_{number}_avr.py').is_file()
    ]
    assert not missing, '\n'.join(missing)
