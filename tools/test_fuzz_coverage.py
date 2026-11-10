# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The nightly fuzz coverage report must not lie.

tools/fuzz_coverage.py renders one gcovr detailed JSON summary per fuzz
target into a markdown report: a score row per target and the files each
target's seed corpus reaches. These tests pin that contract from synthetic
summaries, including the failure modes: a missing or malformed summary
fails with a precise message instead of a silent gap.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COVERAGE = ROOT / 'tools' / 'fuzz_coverage.py'


def gcovr_summary(covered, total):
    # The shape gcovr --json writes: one count per executable line.
    return {
        'gcovr/format_version': '0.9',
        'files': [
            {
                'file': 'core/src/crc.c',
                'lines': [
                    {'line_number': i, 'count': 1 if i <= covered else 0}
                    for i in range(1, total + 1)
                ],
            }
        ],
    }


def run_coverage(directory, pairs, extra=()):
    args = [sys.executable, str(COVERAGE)]
    for target, summary in pairs:
        path = directory / f'{target}.json'
        path.write_text(json.dumps(summary), encoding='utf-8')
        args += ['--target', target, '--summary', str(path)]
    out = directory / 'report.md'
    args += ['--out', str(out), *extra]
    result = subprocess.run(args, capture_output=True, text=True, check=False)
    return result, out.read_text(encoding='utf-8') if out.is_file() else ''


def test_each_target_gets_a_score_row(tmp_path):
    result, report = run_coverage(
        tmp_path,
        [('framer', gcovr_summary(90, 100)), ('codec', gcovr_summary(45, 50))],
    )
    assert result.returncode == 0, result.stderr
    assert 'framer' in report
    assert 'codec' in report
    assert '90%' in report
    assert '45/50' in report


def test_the_report_names_every_file_a_target_reaches(tmp_path):
    result, report = run_coverage(tmp_path, [('framer', gcovr_summary(90, 100))])
    assert result.returncode == 0, result.stderr
    assert 'core/src/crc.c' in report


def test_the_report_table_has_matching_header_and_row_widths(tmp_path):
    result, report = run_coverage(tmp_path, [('framer', gcovr_summary(90, 100))])
    assert result.returncode == 0, result.stderr
    rows = [line for line in report.splitlines() if line.startswith('|')]
    assert len(rows) > 2, report
    widths = {len(row.split('|')) for row in rows}
    assert len(widths) == 1, rows


def test_a_missing_summary_fails_naming_its_path(tmp_path):
    out = tmp_path / 'report.md'
    result = subprocess.run(
        [
            sys.executable,
            str(COVERAGE),
            '--target',
            'framer',
            '--summary',
            str(tmp_path / 'absent.json'),
            '--out',
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert 'absent.json' in result.stderr


def test_a_malformed_summary_fails_naming_its_path(tmp_path):
    bad = tmp_path / 'framer.json'
    bad.write_text('{not json', encoding='utf-8')
    out = tmp_path / 'report.md'
    result = subprocess.run(
        [
            sys.executable,
            str(COVERAGE),
            '--target',
            'framer',
            '--summary',
            str(bad),
            '--out',
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert 'framer.json' in result.stderr


def test_a_target_with_no_measured_lines_fails_loudly(tmp_path):
    result, _ = run_coverage(tmp_path, [('framer', gcovr_summary(0, 0))])
    assert result.returncode != 0
    assert 'framer' in result.stderr


def test_an_unpaired_target_fails_with_a_precise_message(tmp_path):
    out = tmp_path / 'report.md'
    result = subprocess.run(
        [
            sys.executable,
            str(COVERAGE),
            '--target',
            'framer',
            '--out',
            str(out),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert 'framer' in result.stderr
