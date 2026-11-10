# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Flake8 checks for the generator, run only by the advisory lint job.

Ruff is the authoritative Python formatter and linter: this expectation
filters two ament codes that contradict it, by decision (#564). I100 wants the
google import order, which ruff isort rejects with I001 on every site; E203
forbids the Black slice spacing that ruff format restores on every site. No
noqa suppressions: the code stays ruff-formatted and the filter below records
the divergence instead of failing on it.
"""

import os
import re

import pytest

# Ament codes kept out of the expectation because they contradict ruff (#564).
DIVERGED_CODES = frozenset({'E203', 'I100'})
CODE_PATTERN = re.compile(r': ([A-Z]+\d+) ')


def _is_diverged(error: str) -> bool:
    """Tell whether an ament_flake8 error line reports a diverged code."""
    match = CODE_PATTERN.search(error)
    return match is not None and match.group(1) in DIVERGED_CODES


@pytest.mark.flake8
@pytest.mark.linter
def test_flake8():
    if os.environ.get('R2S_AMENT_LINT') != '1':
        pytest.skip('the ament linters run only in the advisory CI job')
    from ament_flake8.main import main_with_errors

    rc, errors = main_with_errors(argv=[])
    remaining = [error for error in errors if not _is_diverged(error)]
    assert not remaining, f'Found {len(remaining)} code style errors / warnings:\n' + '\n'.join(
        remaining
    )
