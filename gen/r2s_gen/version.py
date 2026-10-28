"""The versions r2s-gen reports: its own, and the one the wire speaks.

The release cut writes the repository version into this file next to VERSION,
so the wheel, the ROS manifest and VERSION always state the same number, and a
test fails when they drift apart.
"""

__version__ = "0.0.0"
PROTOCOL_VERSION = 2
