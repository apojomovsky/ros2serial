# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Every posix firmware leg runs the scenario files its shape can serve.

The C firmware and the runtime C++ firmware serve the whole list. The two
narrower C++ firmwares, the one carrying the services and the static one,
run the short list: scenarios 1 to 4 plus 13 and 14, and each scenario
skips a firmware that lacks the parameter table or the services it needs.
These tests read the same list files and the same selection the runner
does, so a leg the runner cannot see fails here.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_SH = ROOT / 'tests' / 'e2e' / 'run.sh'
SHORT = [
    'test_scenario_1_posix',
    'test_scenario_2_posix',
    'test_scenario_3_posix',
    'test_scenario_4_posix',
    'test_scenario_13_posix',
    'test_scenario_14_posix',
]
# One firmware per leg: what it announces and serves is what its leg covers.
FIRMWARES = {
    'static': 'r2s_posix_firmware',
    'runtime': 'r2s_posix_runtime_firmware',
    'services_cpp': 'r2s_posix_services_firmware',
    'static_cpp': 'r2s_posix_static_firmware',
}
NARROW = ('services_cpp', 'static_cpp')


def entries(name):
    """The scenario files of one list file, in order, without blanks or comments."""
    return [
        line
        for line in (ROOT / 'tests' / 'e2e' / name).read_text().splitlines()
        if line and not line.startswith('#')
    ]


def legs():
    """The legs the runner walks, in order."""
    return entries('legs.list')


def leg_block(needle):
    """The run.sh case block over $leg whose body holds needle."""
    lines = RUN_SH.read_text().splitlines()
    for start, line in enumerate(lines):
        if line.strip() != 'case $leg in':
            continue
        end = next(i for i in range(start, len(lines)) if lines[i].strip() == 'esac')
        body = '\n'.join(lines[start : end + 1])
        if needle in body:
            return body
    raise AssertionError(f'run.sh carries no case block holding {needle!r}')


def files_for(leg):
    """The file list the runner selects for one leg, through its own case block."""
    script = (
        f'root={ROOT}\nleg={leg}\n'
        + leg_block('scenarios-short.list')
        + '\n'
        + next(
            line.strip()
            for line in RUN_SH.read_text().splitlines()
            if line.strip().startswith('files=$(grep')
        )
        + '\necho "$files"\n'
    )
    proc = subprocess.run(['bash', '-c', script], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.split()


def test_the_leg_list_names_one_firmware_shape_per_leg():
    assert legs() == list(FIRMWARES)


def test_the_runner_points_each_leg_at_its_firmware():
    mapping = leg_block('R2S_E2E_FW=')
    for leg, firmware in FIRMWARES.items():
        assert f'{leg}) export R2S_E2E_FW=$build/ports/posix/{firmware} ;;' in mapping


def test_every_leg_firmware_is_a_build_target():
    targets = ' '.join((ROOT / 'ports' / 'posix' / 'CMakeLists.txt').read_text().split())
    for firmware in FIRMWARES.values():
        assert f'add_executable({firmware} ' in targets, firmware


def test_the_short_list_covers_the_narrow_legs():
    assert entries('scenarios-short.list') == SHORT


def test_the_narrow_legs_run_the_short_list():
    for leg in NARROW:
        assert files_for(leg) == SHORT


def test_the_wide_legs_run_the_whole_list():
    full = entries('scenarios.list')
    for leg in ('static', 'runtime'):
        assert files_for(leg) == full


def test_the_short_list_is_part_of_the_whole_list():
    full = entries('scenarios.list')
    assert [name for name in SHORT if name in full] == SHORT
