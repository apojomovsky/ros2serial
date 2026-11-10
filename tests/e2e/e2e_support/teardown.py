# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Shared teardown for the launch_testing scenarios.

Every scenario shuts its ROS context down in tearDownClass. Under load the
Zenoh session close can time out after the scenario passed, which must not
fail the run; anything else still raises.
"""

import sys


def is_session_close_timeout(error):
    """Whether error is the known slow session close, not a real failure."""
    message = str(error).lower()
    return isinstance(error, RuntimeError) and 'session' in message and 'close' in message


def shutdown(shutdown_fn=None):
    """Shut the ROS context down, logging a session-close timeout."""
    if shutdown_fn is None:
        import rclpy

        shutdown_fn = rclpy.shutdown
    try:
        shutdown_fn()
    except RuntimeError as error:
        if not is_session_close_timeout(error):
            raise
        print(
            'r2s-e2e: ignoring zenoh session-close timeout in teardown: ' + str(error),
            file=sys.stderr,
        )
