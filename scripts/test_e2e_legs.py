"""The ros compat leg runs scenarios 1 to 4, like the static C++ leg.

Compat is a side path, so it must not cost every run a fourth full
leg. The runtime legs keep the whole file list. These tests read the
same list files as the runner, so a list the runner cannot see fails here.
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
]


def entries(name):
    """The scenario files of one list file, in order, without blanks or comments."""
    return [
        line
        for line in (ROOT / 'tests' / 'e2e' / name).read_text().splitlines()
        if line and not line.startswith('#')
    ]


def files_for(leg):
    """The file list the runner selects for one leg, through its own case block."""
    script = f'leg={leg}\n' + leg_block() + '\necho "$files"\n'
    proc = subprocess.run(['bash', '-c', script], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip()


def leg_block():
    """The runner's leg-to-files selection, exactly as run.sh carries it."""
    lines = RUN_SH.read_text().splitlines()
    starts = [i for i, line in enumerate(lines) if 'case $leg in' in line]
    for start in starts:
        end = next(i for i in range(start, len(lines)) if lines[i].strip() == 'esac')
        block = '\n'.join(lines[start : end + 1])
        if 'scenarios-compat.list' in block and 'scenarios.list' in block:
            files = next(line for line in lines[end + 1 :] if line.strip().startswith('files='))
            return f'root={ROOT}\n' + block + '\n' + files.strip() + '\n'
    raise AssertionError('run.sh carries no leg-to-list block')


def test_the_compat_list_covers_scenarios_1_to_4():
    assert entries('scenarios-compat.list') == SHORT


def test_the_runtime_list_starts_with_scenarios_1_to_4():
    full = entries('scenarios.list')
    assert full[:4] == SHORT
    assert 'test_scenario_6_posix' in full


def test_the_runner_reads_the_leg_list():
    assert 'tests/e2e/legs.list' in RUN_SH.read_text()


def test_compat_legs_cover_scenarios_1_to_4():
    for leg in ('ros', 'static_cpp'):
        assert files_for(leg).split() == SHORT


def test_runtime_legs_run_the_whole_list():
    full = files_for('runtime').split()
    assert full[:4] == SHORT
    assert 'test_scenario_6_posix' in full
