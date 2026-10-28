"""Scenario 1: the handshake announces the firmware topics in time.

The bridge is up before the firmware boots, so the 3 s budget measures the
handshake from the MCU's HELLO, not node startup. The harness boots when the
description is generated, so the boot HELLO predates the bridge; the test
measures from bridge-up instead, when the parameter service answers, which
excludes node startup as the posix spawn does. The buffered HELLO is what
the bridge reads.
"""

import os
import tempfile
import time
import unittest

import launch_testing.markers
import rclpy
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support

# covers: SES-1, SES-2, SES-5, SES-6, SES-13, SES-16, SES-17

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-avr-1-")
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


class TestScenario1(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_handshake_announces_topics_within_3s_of_hello(self):
        node = rclpy.create_node("e2e_s1_avr_probe")
        try:
            bridge_support.wait_for_bridge(node)
            # The boot HELLO predates the bridge, so the budget runs from
            # bridge-up, when the parameter service answers.
            _SPAWNED.ready_at = time.monotonic()
            took = bridge_support.wait_for_topics(node, ANNOUNCED, _SPAWNED.ready_at, TOPIC_BUDGET, "announced topics")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
        finally:
            node.destroy_node()
