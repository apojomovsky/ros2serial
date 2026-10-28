"""Scenario 1: the handshake announces the firmware topics in time.

The bridge is up before the firmware boots, so the 3 s budget measures the
handshake from the MCU's HELLO, not node startup.
"""

import os
import tempfile
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from e2e_support import bridge as bridge_support
from e2e_support import firmware
from e2e_support.relay import PtyRelay

# covers: SES-1, SES-2, SES-5, SES-6, SES-13, SES-16, SES-17

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-1-")
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


class TestScenario1(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_handshake_announces_topics_within_3s_of_hello(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s1_probe")
        try:
            bridge_support.wait_for_bridge(node)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            took = bridge_support.wait_for_topics(node, ANNOUNCED, _SPAWNED.ready_at, TOPIC_BUDGET, "announced topics")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
        finally:
            node.destroy_node()
