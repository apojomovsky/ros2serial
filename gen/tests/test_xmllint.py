# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""XML checks for the generator, run only by the advisory lint job."""

import os

import pytest


@pytest.mark.linter
@pytest.mark.xmllint
def test_xmllint():
    if os.environ.get('R2S_AMENT_LINT') != '1':
        pytest.skip('the ament linters run only in the advisory CI job')
    from ament_xmllint.main import main

    rc = main(argv=[])
    assert rc == 0, 'Found errors'
