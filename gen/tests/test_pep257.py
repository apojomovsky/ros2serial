"""PEP 257 checks for the generator, run only by the advisory lint job."""

import os

import pytest


@pytest.mark.linter
@pytest.mark.pep257
def test_pep257():
    if os.environ.get("R2S_AMENT_LINT") != "1":
        pytest.skip("the ament linters run only in the advisory CI job")
    from ament_pep257.main import main

    rc = main(argv=[])
    assert rc == 0, "Found code style errors / warnings"
