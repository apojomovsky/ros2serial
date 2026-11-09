# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""r2s-gen: the ros2serial code generator.

From message definitions and a config to firmware sources.
"""

from r2s_gen.version import PROTOCOL_VERSION
from r2s_gen.version import __version__

__all__ = ['PROTOCOL_VERSION', '__version__']
