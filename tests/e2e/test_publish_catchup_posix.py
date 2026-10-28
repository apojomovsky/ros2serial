"""The publisher keeps its 50 Hz count across a host scheduling stall.

SIGSTOP freezes the firmware for half a second mid-window; a publisher
that sends at most one frame per tick loses about 25 of the 500 messages,
while one that owes every missed tick stays whole. The bridge and the
probe proved lossless for this flow, so the count below names the
firmware side.
"""

import os
import signal
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

RATE_HZ = 50
WINDOW_S = 10.0
# The scenario 2 floor: at least 99% of the 500 messages with correct values.
RECEIVE_FLOOR = 495
LOSS_BUDGET = 5
# The stall lands two seconds into the window and lasts half a second, about
# 25 ticks: without catch-up the count drops to about 474, with it it stays
# at the unloaded 499.
STALL_AT_S = 2.0
STALL_S = 0.5

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-catchup-")
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


class TestPublishCatchup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_stalled_publisher_keeps_its_count(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_catchup_probe")
        try:
            bridge_support.wait_for_bridge(node)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", str(RATE_HZ), "--no-log-text"))
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            received = []
            node.create_subscription(Float32, "battery", lambda msg: received.append(msg.data), best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            start = time.monotonic()
            while time.monotonic() < start + STALL_AT_S:
                rclpy.spin_once(node, timeout_sec=0.5)
            _SPAWNED.proc.send_signal(signal.SIGSTOP)
            try:
                time.sleep(STALL_S)
            finally:
                _SPAWNED.proc.send_signal(signal.SIGCONT)
            window_end = start + WINDOW_S
            while time.monotonic() < window_end:
                rclpy.spin_once(node, timeout_sec=0.5)
            checks.check_float_sequence(list(received), RECEIVE_FLOOR, LOSS_BUDGET, "battery at 50 Hz across a stall")
        finally:
            node.destroy_node()
