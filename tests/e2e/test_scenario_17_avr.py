# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 17: a custom message, service and action from the interface package
reach a firmware built from the generated static API.

The firmware is `examples/custom_interfaces/custom_firmware`: `r2s-gen`
emits its C sources from the example's `r2s.yaml` against the interface
package beside it, so the types on the wire are the package's own
definitions, not bundled ones. The scenario publishes and reads the custom
message, calls the custom service, and sends a goal to the custom action,
proving both directions for every one of the three kinds.

The bridge opens a stable symlink, and every test respawns the board on it,
the way scenario 16 does: stopping a harness drops the bridge to
DISCONNECTED, and the fresh harness re-handshakes on the same path.
"""

# covers: SVR-4, SVR-5, SVR-6, SVR-7, ACT-2, ACT-3, ACT-4, ACT-8

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from action_msgs.msg import GoalStatus
from custom_interfaces.action import Run
from custom_interfaces.msg import SensorReading
from custom_interfaces.srv import Calibrate
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from e2e_support import teardown
from rclpy.action import ActionClient
from rclpy.qos import qos_profile_sensor_data

ANNOUNCED = ['/reading']
MESSAGE_BUDGET = 15.0
SERVICE_TIMEOUT = 20.0
CALL_TIMEOUT = 20.0
ACTION_TIMEOUT = 30.0
GOAL_TIMEOUT = 15.0
FEEDBACK_TIMEOUT = 20.0
RESULT_TIMEOUT = 25.0

_SCRATCH = None
_LINK = None
_SPAWNED = []
_PARAMS = None


def _prepare():
    global _SCRATCH, _LINK, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-avr-17-')
    _LINK = Path(_SCRATCH.name) / 'bridge_port'
    _PARAMS = bridge_support.example_params_file()


def _spawn_board():
    """Stop whatever ran and start a fresh board on the bridge's link."""
    for spawned in _SPAWNED:
        spawned.stop()
    board = os.environ.get('R2S_E2E_BOARD', 'uno')
    spawned = avr_support.spawn(board, flavor='custom')
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


class TestScenario17(unittest.TestCase):
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
            lambda: rclpy.spin_once(node, bridge_support.DISCOVERY_SPIN),
        )
        # The test that spawned this board stops it, whatever order unittest
        # runs the tests in: launch_testing never calls tearDownModule, and
        # the stop summary is what records the painted stack peak.
        self.addCleanup(lambda: self.assertEqual(spawned.stop(), 0, 'the harness stopped dirty'))
        return spawned

    def test_the_custom_message_arrives(self):
        """The firmware publishes the interface package's message at 10 Hz."""
        node = rclpy.create_node('e2e_s17_msg')
        try:
            # The MCU's topics are best effort, so the probe asks for the
            # same: a reliable subscription would never receive one.
            received = []
            node.create_subscription(
                SensorReading, '/reading', received.append, qos_profile_sensor_data
            )
            self._handshake(node)
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

    def test_the_custom_service_answers(self):
        """A ROS call reaches the firmware's served service and returns its answer."""
        node = rclpy.create_node('e2e_s17_srv')
        try:
            self._handshake(node)
            client = node.create_client(Calibrate, '/calibrate')
            try:
                self.assertTrue(
                    client.wait_for_service(timeout_sec=SERVICE_TIMEOUT),
                    'the calibrate service never appeared',
                )
                request = Calibrate.Request()
                request.target = 2.5
                future = client.call_async(request)
                deadline = time.monotonic() + CALL_TIMEOUT
                while not future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(
                        time.monotonic(), deadline, 'the calibrate call never answered'
                    )
                response = future.result()
                self.assertTrue(response.accepted, 'the firmware refused a positive target')
                self.assertEqual(
                    response.note, 'calibrated', 'the answer carries the firmware text'
                )
            finally:
                node.destroy_client(client)
        finally:
            node.destroy_node()

    def test_the_custom_action_runs(self):
        """A ROS goal reaches the firmware's action, sends feedback and a result."""
        node = rclpy.create_node('e2e_s17_action')
        try:
            self._handshake(node)
            client = ActionClient(node, Run, '/run')
            try:
                self.assertTrue(
                    client.wait_for_server(timeout_sec=ACTION_TIMEOUT),
                    'the run action server never appeared',
                )
                feedbacks = []
                goal = Run.Goal()
                goal.order = 4.0
                send_future = client.send_goal_async(
                    goal, feedback_callback=lambda msg: feedbacks.append(msg.feedback)
                )
                deadline = time.monotonic() + GOAL_TIMEOUT
                while not send_future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the goal never answered')
                handle = send_future.result()
                self.assertTrue(handle.accepted, 'the firmware refused the goal')
                deadline = time.monotonic() + FEEDBACK_TIMEOUT
                while not feedbacks:
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'no feedback arrived')
                self.assertAlmostEqual(feedbacks[0].progress, 4.0, places=3)
                result_future = handle.get_result_async()
                deadline = time.monotonic() + RESULT_TIMEOUT
                while not result_future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the goal never finished')
                result = result_future.result()
                self.assertEqual(result.status, GoalStatus.STATUS_SUCCEEDED)
                self.assertAlmostEqual(result.result.value, 4.0, places=3)
                self.assertEqual(result.result.note, 'done')
            finally:
                node.destroy_client(client)
        finally:
            node.destroy_node()
