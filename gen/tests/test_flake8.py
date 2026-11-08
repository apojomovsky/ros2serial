"""Flake8 checks for the generator, run only by the advisory lint job."""

import os

import pytest


@pytest.mark.flake8
@pytest.mark.linter
def test_flake8():
    if os.environ.get("R2S_AMENT_LINT") != "1":
        pytest.skip("the ament linters run only in the advisory CI job")
    from ament_flake8.main import main_with_errors

    rc, errors = main_with_errors(argv=[])
    assert rc == 0, "Found %d code style errors / warnings:\n" % len(errors) + "\n".join(errors)
