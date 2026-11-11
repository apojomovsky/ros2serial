# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 16: a ROS goal reaches the example action sketch and runs.

The arduino_action sketch serves the spin action as Float32 goal, result
and feedback against the test_msgs/action/Fibonacci type name. The ROS
action client sends a goal with order 2 and the MCU accepts it, proving
the ROS-initiated goal crossed the wire and decoded. The MCU then runs
its three-step sequence and answers succeed with its Float32 result, and
the bridge drops that payload: a 4-byte float is a zero-count sequence
with three trailing bytes under the Fibonacci result layout, which
[ENC-18] refuses, so the ROS goal ends aborted. The abort itself proves
the RESULT crossed back; the content mapping is the documented Float32
stand-in, not the wiring under test. The sketch announces no topics, so
the handshake check waits for the connected link alone.
"""

import os
import tempfile
import time
import unittest

import launch_testing.markers
import rclpy
from action_msgs.msg import GoalStatus
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from e2e_support import teardown
from rclpy.action import ActionClient
from test_msgs.action import Fibonacci

# covers: ACT-2, ACT-3, ACT-4, ACT-6, ACT-7, ACT-8

TOPIC_BUDGET = 3.0
SERVER_TIMEOUT = 20.0
GOAL_TIMEOUT = 20.0
RESULT_TIMEOUT = 60.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-avr-16-')
    _SPAWNED = avr_support.spawn(os.environ.get('R2S_E2E_BOARD', 'uno'), flavor='action')
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_SPAWNED.pty, _PARAMS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


class TestScenario16(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_ros_goal_runs_on_the_mcu(self):
        node = rclpy.create_node('e2e_s16_probe')
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            received.clear()
            # The boot HELLO predates the bridge, so the handshake budget
            # runs from bridge-up, when the parameter service answers.
            _SPAWNED.ready_at = time.monotonic()
            took = checks.wait_for_handshake(
                received,
                [],
                _SPAWNED.ready_at,
                TOPIC_BUDGET,
                'handshake',
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
                clock=time.monotonic,
            )
            self.assertLessEqual(took, TOPIC_BUDGET, 'handshake took longer than 3 s')
            client = ActionClient(node, Fibonacci, 'spin')
            try:
                self.assertTrue(
                    client.wait_for_server(timeout_sec=SERVER_TIMEOUT),
                    'the spin action server never appeared',
                )
                goal = Fibonacci.Goal()
                goal.order = 2
                send_future = client.send_goal_async(goal)
                deadline = time.monotonic() + GOAL_TIMEOUT
                while not send_future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the goal never reached the MCU')
                handle = send_future.result()
                self.assertTrue(handle.accepted, 'the MCU rejected the goal')
                result_future = handle.get_result_async()
                deadline = time.monotonic() + RESULT_TIMEOUT
                while not result_future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the MCU never answered the goal')
                result = result_future.result()
                # The MCU ran its sequence and answered succeed: the bridge
                # drops the Float32 stand-in payload under [ENC-18], so the
                # ROS goal ends aborted instead of succeeding. A disconnect
                # aborts the same way, so the link must still read
                # connected for the abort to prove the RESULT crossed.
                self.assertEqual(
                    result.status, GoalStatus.STATUS_ABORTED, 'the goal ended elsewhere'
                )
                self.assertEqual(
                    received[-1].status[0].message, 'connected', 'the link dropped mid-goal'
                )
                # launch_testing never calls tearDownModule, so the harness
                # stops here: its stop summary carries the painted peak.
                self.assertEqual(_SPAWNED.stop(), 0, 'the harness stopped dirty')
            finally:
                client.destroy()
        finally:
            node.destroy_node()
