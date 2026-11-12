# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 15: a ROS service call reaches the example service sketch and back, both ways.

The arduino_service sketch serves reset and calls trigger (both std_srvs
Trigger). The ROS client calls /reset and reads the MCU's answer, proving
the ROS-initiated round trip with MCU-generated content. The ROS server on
/trigger answers the MCU's periodic call, proving the MCU-initiated round
trip the same way: the sketch calls every few seconds while connected, so
a call lands after the server exists no matter when the link came up. The
sketch announces no topics, so the handshake check waits for the
connected link alone.
"""

import os
import tempfile
import time
import unittest

import launch_testing.markers
import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from e2e_support import teardown
from std_srvs.srv import Trigger

# covers: SVR-4, SVR-5, SVR-6, SVR-7

TOPIC_BUDGET = 3.0
SERVICE_TIMEOUT = 20.0
CALL_TIMEOUT = 20.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-avr-15-')
    _SPAWNED = avr_support.spawn(os.environ.get('R2S_E2E_BOARD', 'uno'), flavor='service')
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


class TestScenario15(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_service_calls_round_trip_both_directions(self):
        node = rclpy.create_node('e2e_s15_probe')
        try:
            bridge_support.wait_for_bridge(node)
            # The server exists before the handshake finishes, so the
            # bridge answers the MCU's calls from the first one on.
            served = []
            server = node.create_service(
                Trigger,
                '/trigger',
                lambda request, response: served.append(request) or self._answer_trigger(response),
            )
            try:
                received = []
                node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
                deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
                while not received:
                    rclpy.spin_once(node, timeout_sec=0.5)
                    self.assertLess(
                        time.monotonic(), deadline, 'no /diagnostics flow from the bridge'
                    )
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
                # The ROS call reaches the MCU and comes back with content
                # only the firmware produces.
                client = node.create_client(Trigger, '/reset')
                try:
                    self.assertTrue(
                        client.wait_for_service(timeout_sec=SERVICE_TIMEOUT),
                        'the reset service never appeared',
                    )
                    future = client.call_async(Trigger.Request())
                    deadline = time.monotonic() + CALL_TIMEOUT
                    while not future.done():
                        rclpy.spin_once(node, timeout_sec=0.2)
                        self.assertLess(
                            time.monotonic(), deadline, 'the reset call never answered'
                        )
                    response = future.result()
                    self.assertTrue(response.success, 'the MCU refused reset')
                    self.assertEqual(response.message, 'ok', 'the MCU answer carries its own text')
                finally:
                    node.destroy_client(client)
                # The MCU's call reaches the ROS server, whose answer reaches
                # the MCU: the server saw the call.
                deadline = time.monotonic() + CALL_TIMEOUT
                while not served:
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the MCU never called trigger')
                # launch_testing never calls tearDownModule, so the harness
                # stops here: its stop summary carries the painted peak.
                self.assertEqual(_SPAWNED.stop(), 0, 'the harness stopped dirty')
            finally:
                node.destroy_service(server)
        finally:
            node.destroy_node()

    @staticmethod
    def _answer_trigger(response):
        response.success = True
        response.message = 'server-ok'
        return response
