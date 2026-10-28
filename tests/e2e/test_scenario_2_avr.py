"""Scenario 2: the MCU publishes Float32 at 50 Hz.

The host collects 500 messages with the counter values intact: no
corruption, no reordering, at most 5 lost frames. The count replaces the
posix time window, so simulated time running off the wall clock cannot skew
the gate; the span check counts the lost frames against the same 1%.
"""

import os
import tempfile
import time
import unittest

import launch_testing.markers
import rclpy
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

RECEIVE_COUNT = 500
LOSS_BUDGET = 5
FLOW_TIMEOUT = 120.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-avr-2-")
    _SPAWNED = avr_support.spawn(os.environ.get("R2S_E2E_BOARD", "uno"))
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


class TestScenario2(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_50hz_flow_arrives_nearly_whole_with_correct_values(self):
        node = rclpy.create_node("e2e_s2_avr_probe")
        try:
            bridge_support.wait_for_bridge(node)
            # The bridge publishes MCU topics best effort, so the probe
            # subscribes best effort too; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            received = []
            node.create_subscription(Float32, "battery", lambda msg: received.append(msg.data), best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            checks.collect_until(
                received,
                RECEIVE_COUNT,
                time.monotonic() + FLOW_TIMEOUT,
                "battery at 50 Hz",
                lambda: rclpy.spin_once(node, timeout_sec=0.5),
            )
            checks.check_float_sequence(list(received), RECEIVE_COUNT, LOSS_BUDGET, "battery at 50 Hz")
        finally:
            node.destroy_node()
