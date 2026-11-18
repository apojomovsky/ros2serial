# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 18: a custom type reaches a firmware through the X-macro list.

The message part of scenario 17 on the other route. The firmware is
`examples/arduino_xmacro_custom`: its `R2S_TOPICS` row names the interface
package's own message, and the library beside it comes from `r2s-gen types`,
so the type on the wire is the package's definition rather than the bundled
snapshot's. The static route's scenario publishes the same message from a
generated table; this one proves the X-macro expansion announces and encodes
it identically.

The bridge opens a stable symlink and the test respawns the board on it, the
way scenarios 16 and 17 do: stopping a harness drops the bridge to
DISCONNECTED, and the fresh harness re-handshakes on the same path.
"""

# covers: SES-9, SES-18, SES-31

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from custom_interfaces.msg import SensorReading
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from e2e_support import teardown
from rclpy.qos import qos_profile_sensor_data

ANNOUNCED = ['/reading']
MESSAGE_BUDGET = 15.0

_SCRATCH = None
_LINK = None
_SPAWNED = []
_PARAMS = None


def _prepare():
    global _SCRATCH, _LINK, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-avr-18-')
    _LINK = Path(_SCRATCH.name) / 'bridge_port'
    _PARAMS = bridge_support.example_params_file()


def _spawn_board():
    """Stop whatever ran and start a fresh board on the bridge's link."""
    for spawned in _SPAWNED:
        spawned.stop()
    board = os.environ.get('R2S_E2E_BOARD', 'uno')
    spawned = avr_support.spawn(board, flavor='custom_xmacro')
    if _LINK.is_symlink() or _LINK.exists():
        _LINK.unlink()
    os.symlink(spawned.pty, _LINK)
    _SPAWNED.append(spawned)
    spawned.ready_at = time.monotonic()
    return spawned


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    _spawn_board()
    return bridge_support.launch_bridge_only(str(_LINK), _PARAMS)


def tearDownModule():
    for spawned in _SPAWNED:
        spawned.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


class TestScenario18(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def _handshake(self, node):
        """Respawn the board and wait for the link, topics and graph."""
        received = []
        node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
        deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
        while not received:
            rclpy.spin_once(node, timeout_sec=0.5)
            self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
        received.clear()
        spawned = _spawn_board()
        # The wait asserts its own budget, so it is the handshake check.
        checks.wait_for_handshake(
            received,
            [],
            spawned.ready_at,
            bridge_support.HANDSHAKE_TIMEOUT,
            'link',
            lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
            clock=time.monotonic,
        )
        bridge_support.wait_for_topics(
            node,
            ANNOUNCED,
            time.monotonic(),
            checks.DISCOVERY_TIMEOUT,
            'announced topics',
            lambda: rclpy.spin_once(node, timeout_sec=bridge_support.DISCOVERY_SPIN),
        )
        # The test that spawned this board stops it, whatever order unittest
        # runs the tests in: launch_testing never calls tearDownModule, and
        # the stop summary is what records the painted stack peak.
        self.addCleanup(lambda: self.assertEqual(spawned.stop(), 0, 'the harness stopped dirty'))
        return spawned

    def test_the_custom_message_arrives(self):
        """The X-macro firmware publishes the interface package's message."""
        node = rclpy.create_node('e2e_s18_msg')
        try:
            self._handshake(node)
            # The probe subscribes after the respawn: the board the module
            # spawned is stopped by the handshake, and its stream would read
            # as a counter reset rather than as a decode failure. The MCU's
            # topics are best effort, so the probe asks for the same: a
            # reliable subscription would never receive one.
            received = []
            node.create_subscription(
                SensorReading, '/reading', received.append, qos_profile_sensor_data
            )
            deadline = time.monotonic() + MESSAGE_BUDGET
            while len(received) < 3:
                rclpy.spin_once(node, timeout_sec=0.2)
                self.assertLess(time.monotonic(), deadline, 'the custom message never arrived')
            # The firmware counts its publishes in status and derives
            # temperature from it, so the two fields together prove a decoded
            # payload rather than a zeroed frame. The subscription starts
            # mid-stream, so the first status is whatever the counter reached.
            for reading in received:
                self.assertAlmostEqual(reading.temperature, float(reading.status) / 10.0, places=3)
            statuses = [reading.status for reading in received]
            self.assertEqual(statuses, sorted(statuses), 'the counter went backwards')
        finally:
            node.destroy_node()
