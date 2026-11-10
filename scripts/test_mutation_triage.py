# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The nightly mutation triage report gates the nightly, but it must not lie.

scripts/mutation_triage.py merges the normalized Mull and mutmut results
into one markdown report: a score per component against the section 6
floors, the surviving mutants that need a test, and the acknowledged
equivalents with their one-line reasons. With --gate a measured score
below its floor fails the nightly run. These tests pin that contract
from synthetic results, including the acceptance case: a deliberately
weakened check that survives must show up in the report.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRIAGE = ROOT / 'scripts' / 'mutation_triage.py'


def run_triage(directory, mull, mutmut, equivalent=(), extra=()):
    (directory / 'mull.json').write_text(json.dumps(mull), encoding='utf-8')
    (directory / 'mutmut.json').write_text(json.dumps(mutmut), encoding='utf-8')
    body = ''.join(f'- mutant: {mutant!r}\n  reason: {reason}\n' for mutant, reason in equivalent)
    (directory / 'equivalent.yaml').write_text(body, encoding='utf-8')
    out = directory / 'report.md'
    result = subprocess.run(
        [
            sys.executable,
            str(TRIAGE),
            '--mull',
            str(directory / 'mull.json'),
            '--mutmut',
            str(directory / 'mutmut.json'),
            '--equivalent',
            str(directory / 'equivalent.yaml'),
            '--out',
            str(out),
            *extra,
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    return result, out.read_text(encoding='utf-8') if out.is_file() else ''


def weakened_crc_results():
    weakened = {
        'id': 'core:core/src/crc.c:42:cxx_add_to_sub',
        'file': 'core/src/crc.c',
        'line': 42,
        'mutator': 'cxx_add_to_sub',
        'outcome': 'survived',
    }
    killed = {
        'id': 'core:core/src/crc.c:43:cxx_add_to_sub',
        'file': 'core/src/crc.c',
        'line': 43,
        'mutator': 'cxx_add_to_sub',
        'outcome': 'killed',
    }
    mull = {'component': 'core', 'mutants': [weakened, killed]}
    mutmut = {'component': 'gen', 'mutants': []}
    return mull, mutmut


def test_a_weakened_check_shows_up_as_a_surviving_mutant(tmp_path):
    """The acceptance case: a weakened check that survives is triaged."""
    mull, mutmut = weakened_crc_results()
    result, report = run_triage(tmp_path, mull, mutmut)
    assert result.returncode == 0, result.stderr
    assert 'core/src/crc.c:42' in report
    assert 'cxx_add_to_sub' in report
    assert 'needs triage' in report.lower()


def test_an_acknowledged_equivalent_leaves_the_triage_list(tmp_path):
    mull, mutmut = weakened_crc_results()
    equivalent = [
        ('core:core/src/crc.c:42:cxx_add_to_sub', 'the caller masks the bit before it matters')
    ]
    result, report = run_triage(tmp_path, mull, mutmut, equivalent)
    assert result.returncode == 0, result.stderr
    assert 'the caller masks the bit before it matters' in report
    triage = report.lower().split('needs triage', 1)[1] if 'needs triage' in report.lower() else ''
    assert 'core/src/crc.c:42' not in triage


def test_the_report_scores_each_component_against_its_floor(tmp_path):
    mull, mutmut = weakened_crc_results()
    result, report = run_triage(tmp_path, mull, mutmut)
    assert result.returncode == 0, result.stderr
    for component, floor in (('core', '85'), ('cpp', '75'), ('gen', '80'), ('bridge', '70')):
        assert component in report
        assert floor in report


def test_a_score_below_its_floor_never_fails_the_command(tmp_path):
    mull = {'component': 'core', 'mutants': []}
    mutmut = {'component': 'gen', 'mutants': []}
    result, report = run_triage(tmp_path, mull, mutmut)
    assert result.returncode == 0, result.stderr
    assert report.strip() != ''


def test_a_stale_equivalent_entry_is_reported(tmp_path):
    mull, mutmut = weakened_crc_results()
    equivalent = [('core:core/src/crc.c:99:cxx_add_to_sub', 'left over from an old run')]
    result, report = run_triage(tmp_path, mull, mutmut, equivalent)
    assert result.returncode == 0, result.stderr
    assert 'stale' in report.lower()
    assert 'core/src/crc.c:99' in report


def test_a_missing_result_file_fails_with_a_precise_message(tmp_path):
    out = tmp_path / 'report.md'
    result = subprocess.run(
        [
            sys.executable,
            str(TRIAGE),
            '--mull',
            str(tmp_path / 'absent.json'),
            '--mutmut',
            str(tmp_path / 'mutmut.json'),
            '--equivalent',
            str(tmp_path / 'equivalent.yaml'),
            '--out',
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert 'absent.json' in result.stderr


def test_the_report_table_has_matching_header_and_row_widths(tmp_path):
    mull, mutmut = weakened_crc_results()
    result, report = run_triage(tmp_path, mull, mutmut)
    assert result.returncode == 0, result.stderr
    rows = [line for line in report.splitlines() if line.startswith('|')]
    assert len(rows) > 2, report
    widths = {len(row.split('|')) for row in rows}
    assert len(widths) == 1, rows


def half_killed_results():
    killed = {
        'id': 'core:core/src/crc.c:43:cxx_add_to_sub',
        'file': 'core/src/crc.c',
        'line': 43,
        'mutator': 'cxx_add_to_sub',
        'outcome': 'killed',
    }
    survived = {
        'id': 'core:core/src/crc.c:42:cxx_add_to_sub',
        'file': 'core/src/crc.c',
        'line': 42,
        'mutator': 'cxx_add_to_sub',
        'outcome': 'survived',
    }
    mull = {'component': 'core', 'mutants': [survived, killed]}
    mutmut = {'component': 'gen', 'mutants': []}
    return mull, mutmut


def test_the_gate_fails_a_measured_score_below_its_floor(tmp_path):
    mull, mutmut = half_killed_results()
    result, report = run_triage(tmp_path, mull, mutmut, extra=('--gate',))
    assert result.returncode != 0
    assert 'core' in result.stderr
    assert '50%' in result.stderr
    assert '85%' in result.stderr
    assert report.strip() != ''


def test_the_gate_passes_a_score_at_its_floor(tmp_path):
    mutants = [
        {
            'id': f'core:core/src/crc.c:{i}:cxx_add_to_sub',
            'file': 'core/src/crc.c',
            'line': i,
            'mutator': 'cxx_add_to_sub',
            'outcome': 'killed' if i < 17 else 'survived',
        }
        for i in range(20)
    ]
    mull = {'component': 'core', 'mutants': mutants}
    mutmut = {'component': 'gen', 'mutants': []}
    result, _ = run_triage(tmp_path, mull, mutmut, extra=('--gate',))
    assert result.returncode == 0, result.stderr


def test_the_gate_ignores_components_with_no_data(tmp_path):
    mull = {'component': 'core', 'mutants': []}
    mutmut = {'component': 'gen', 'mutants': []}
    result, _ = run_triage(tmp_path, mull, mutmut, extra=('--gate',))
    assert result.returncode == 0, result.stderr


def test_the_gate_reads_its_floors_from_a_file(tmp_path):
    floors = tmp_path / 'floors.yaml'
    floors.write_text(
        'mutation_floors:\n  core: 99\n  cpp: 99\n  gen: 99\n  bridge: 99\n', encoding='utf-8'
    )
    mull, mutmut = half_killed_results()
    result, _ = run_triage(tmp_path, mull, mutmut, extra=('--gate', '--floors', str(floors)))
    assert result.returncode != 0
    assert '99%' in result.stderr


def test_the_gate_rejects_a_floors_file_missing_a_component(tmp_path):
    floors = tmp_path / 'floors.yaml'
    floors.write_text('mutation_floors:\n  core: 85\n', encoding='utf-8')
    mull, mutmut = half_killed_results()
    result, _ = run_triage(tmp_path, mull, mutmut, extra=('--gate', '--floors', str(floors)))
    assert result.returncode != 0
    assert 'cpp' in result.stderr
