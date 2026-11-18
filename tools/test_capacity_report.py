# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The capacity page and the numbers behind it.

The page is generated from committed measurements, so hand-editing a row,
changing the fit, or changing what a build costs the core all fail here
until the measurements are retaken with `make capacity-report`. The unit
cases pin the arithmetic the page quotes: a fit that recovers a known
slope, a capacity that floors, and a builder that emits the shape it names.
"""

import json
from pathlib import Path

import capacity_report

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / 'docs' / 'capacity.md'
MEASUREMENTS = ROOT / 'tools' / 'capacity_measurements.json'


def _measurements() -> dict:
    return capacity_report.load(MEASUREMENTS)


def test_fit_recovers_the_slope_and_the_base():
    runs = [
        {'shape': 'publisher', 'count': n, 'flash': 8000 + 60 * n, 'ram': 500 + 26 * n}
        for n in (1, 2, 4, 8, 16)
    ]
    slope, base = capacity_report.fit(runs, 'ram')
    assert slope == 26
    assert base == 500


def test_fit_needs_two_points():
    runs = [{'shape': 'publisher', 'count': 4, 'flash': 8100, 'ram': 604}]
    assert capacity_report.fit(runs, 'flash') is None


def test_capacity_floors_at_whole_entities():
    # 1000 usable, 400 base, 100 per entity: six fit, not the seven a
    # ceiling would claim.
    assert capacity_report.capacity(100, 400, 1000) == 6
    assert capacity_report.capacity(100, 2000, 1000) == 0


def test_capacity_refuses_a_zero_slope():
    assert capacity_report.capacity(0, 400, 1000) == 0


def test_publisher_sketch_declares_only_publishers():
    text = capacity_report.sketch('runtime', 'publisher', 3)
    assert text.count('r2s::Publisher<') == 3
    assert 'r2s::Subscription<' not in text


def test_subscription_sketch_declares_only_subscriptions():
    text = capacity_report.sketch('runtime', 'subscription', 4)
    assert text.count('r2s::Subscription<') == 4
    assert 'r2s::Publisher<' not in text


def test_xmacro_rows_match_the_count_and_the_mix():
    text = capacity_report.sketch('xmacro', 'mixed', 4)
    assert text.count('X(Publisher,') == 2
    assert text.count('X(Subscription,') == 2


def test_service_sketch_serves_the_count():
    text = capacity_report.sketch('runtime', 'service', 2)
    assert text.count('r2s::Service<') == 2


def test_every_staged_file_exists_in_the_tree():
    missing = [name for name in capacity_report.STAGED_FILES if not (ROOT / name).is_file()]
    assert missing == []


def test_page_quotes_the_method_and_the_allowance():
    page = capacity_report.render_page(_measurements())
    assert 'make capacity-report' in page
    for token in (capacity_report.ALLOWANCE_FIELD, 'R2S_MAX_FRAME'):
        assert token in page


def test_page_tables_every_measured_board():
    page = capacity_report.render_page(_measurements())
    for board in _measurements()['boards']:
        assert f'| {board} |' in page


def test_page_render_is_deterministic():
    measurements = _measurements()
    assert capacity_report.render_page(measurements) == capacity_report.render_page(measurements)


def test_committed_page_matches_the_measurements():
    """A hand-edited row, or a fit change, fails until the page is regenerated."""
    page = PAGE.read_text(encoding='utf-8')
    assert page == capacity_report.render_page(_measurements())


def test_measurements_name_the_tree_they_measured():
    """A core change that moves a byte fails until the numbers are taken again."""
    recorded = _measurements()['inputs']
    assert recorded == capacity_report.input_digest(ROOT)


def test_check_reports_a_stale_page(tmp_path):
    stale = tmp_path / 'capacity.md'
    stale.write_text('hand edited\n', encoding='utf-8')
    assert capacity_report.check(MEASUREMENTS, stale) != []


def test_saved_measurements_round_trip(tmp_path):
    measurements = _measurements()
    path = tmp_path / 'm.json'
    capacity_report.save(measurements, path)
    assert json.loads(path.read_text(encoding='utf-8')) == measurements


def test_committed_measurements_are_written_one_run_per_line():
    """The generated file keeps its compact shape: a reformatted copy is drift."""
    assert MEASUREMENTS.read_text(encoding='utf-8') == capacity_report.dumps(_measurements())
