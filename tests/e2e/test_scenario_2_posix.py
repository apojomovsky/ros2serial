"""Scenario 2: the MCU publishes Float32 at 50 Hz for 10 s.

The host receives at least 99% of the 500 messages with the counter values
intact: no corruption, no reordering, at most 5 lost frames.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from e2e_support import bridge as bridge_support
from e2e_support import checks, firmware
from e2e_support.relay import PtyRelay
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: SES-9, SES-18, SES-31, FRM-4, FRM-5

RATE_HZ = 50
WINDOW_S = 10.0
EXPECTED = int(RATE_HZ * WINDOW_S)
LOSS_BUDGET = EXPECTED // 100

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-2-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = firmware.from_run_script() if "R2S_E2E_FW" in os.environ else firmware.build(scratch)
    _RELAY = PtyRelay()
    _RELAY.start()
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_RELAY.host_port, _PARAMS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _RELAY is not None:
        _RELAY.stop()
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
        global _SPAWNED
        node = rclpy.create_node("e2e_s2_probe")
        try:
            bridge_support.wait_for_bridge(node)
            _SPAWNED = firmware.spawn(
                _FIRMWARE, _RELAY.mcu_port, Path(_SCRATCH.name), ("--rate", str(RATE_HZ), "--no-log-text")
            )
            # The bridge publishes MCU topics best effort, so the probe
            # subscribes best effort too; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            received = []
            node.create_subscription(Float32, "battery", received.append, best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            start = time.monotonic()
            window_end = start + WINDOW_S
            while time.monotonic() < window_end:
                rclpy.spin_once(node, timeout_sec=0.5)
            checks.check_float_sequence(list(received), EXPECTED, LOSS_BUDGET, "battery at 50 Hz")
        finally:
            node.destroy_node()
