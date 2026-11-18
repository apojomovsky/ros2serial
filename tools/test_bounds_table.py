# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The limits table in docs/message-bounds.md comes from `r2s-gen limits`.

The page carries one generated block, and the gate fails while it differs
from what the command prints, so editing `BUNDLE_BOUNDS` without running
`make bounds-table` breaks the docs job instead of leaving stale numbers.
"""

from pathlib import Path

import bounds_table
from r2s_gen import limits

ROOT = Path(__file__).resolve().parent.parent


def page(block: str) -> str:
    """A page carrying one generated block between prose."""
    return f'# Message bounds\n\nProse above.\n\n{block}\nProse below.\n'


def test_the_block_is_the_command_output_between_its_markers():
    block = bounds_table.render_block()

    assert block.startswith(bounds_table.BEGIN + '\n')
    assert block.endswith(bounds_table.END + '\n')
    assert limits.table('jazzy').split('\n', 1)[1] in block


def test_a_page_holding_the_rendered_block_passes():
    assert bounds_table.check(page(bounds_table.render_block())) == []


def test_a_stale_row_is_named_with_the_refresh_command():
    stale = bounds_table.render_block().replace(
        'string        16      BUNDLE_BOUNDS', 'string        32      BUNDLE_BOUNDS', 1
    )

    found = bounds_table.check(page(stale))

    assert len(found) == 1
    assert 'make bounds-table' in found[0]


def test_a_page_without_the_markers_is_named():
    found = bounds_table.check('# Message bounds\n\nNo table here.\n')

    assert len(found) == 1
    assert 'bounds-table: begin' in found[0]


def test_writing_replaces_the_block_and_leaves_the_prose_alone():
    stale = page(bounds_table.render_block().replace('BUNDLE_BOUNDS', 'somewhere else'))

    written = bounds_table.with_block(stale, bounds_table.render_block())

    assert bounds_table.check(written) == []
    assert written.startswith('# Message bounds\n\nProse above.\n\n')
    assert written.endswith('\nProse below.\n')


def test_the_real_page_matches_the_command():
    """Editing BUNDLE_BOUNDS without regenerating fails here, as it fails the gate."""
    text = (ROOT / bounds_table.PAGE).read_text(encoding='utf-8')

    assert bounds_table.check(text) == []
