"""Copyright checks for the generator, run only by the advisory lint job."""

import os

import pytest


@pytest.mark.copyright
@pytest.mark.linter
def test_copyright():
    if os.environ.get('R2S_AMENT_LINT') != '1':
        pytest.skip('the ament linters run only in the advisory CI job')
    from ament_copyright.main import main

    rc = main(argv=[])
    assert rc == 0, 'Found errors'
