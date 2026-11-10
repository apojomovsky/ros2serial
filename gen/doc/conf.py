# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Sphinx entry for the r2s_gen rosdoc2 pages."""

import os
import sys

# The doc build never installs the generator, so autodoc imports it from
# the repository instead: this file sits in gen/doc, and the importable
# package lives one directory up.
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
