"""Scenario 1: the handshake announces the firmware topics in time.

The bridge is up before the firmware boots, so the 3 s budget measures the
handshake from the MCU's HELLO, not node startup. The harness boots when the
description is generated, so the boot HELLO predates the bridge; the test
measures from bridge-up instead, when the parameter service answers, which
excludes node startup as the posix spawn does. The buffered HELLO is what
the bridge reads. Graph discovery of the new topics waits on its own generous
timeout from the handshake, so a slow runner cannot fail a correct handshake.
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
from e2e_support import checks, teardown

# covers: SES-1, SES-2, SES-5, SES-6, SES-13, SES-16, SES-17

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text"]
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix("/") for name in ANNOUNCED]
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
        teardown.shutdown()

    def test_handshake_announces_topics_within_3s_of_hello(self):
        node = rclpy.create_node("e2e_s1_avr_probe")
        try:
            bridge_support.wait_for_bridge(node)
            # The handshake wait below observes bridge state, not DDS
            # discovery of the new topics.
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            received.clear()
            # The boot HELLO predates the bridge, so the handshake budget runs
            # from bridge-up, when the parameter service answers.
            _SPAWNED.ready_at = time.monotonic()
            # /diagnostics publishes at 1 Hz, so the observed time can lag the
            # real handshake by one period; the bound still holds because the
            # handshake traffic itself is tens of milliseconds.
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS) if received else ["link silent"]
                took = time.monotonic() - _SPAWNED.ready_at
                if not missing:
                    break
                self.assertLess(took, TOPIC_BUDGET, f"handshake still missing {missing} after {took:.1f} s")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
            # Discovery starts when the bridge creates its endpoints at the
            # handshake, so the generous timeout runs from there, not from
            # bridge-up: a slow handshake must not spend it.
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node, ANNOUNCED, handshake_done_at, checks.DISCOVERY_TIMEOUT, "announced topics"
            )
            # launch_testing never calls tearDownModule, so the harness
            # stops here: its stop summary carries the painted peak.
            self.assertEqual(_SPAWNED.stop(), 0, "the harness stopped dirty")
        finally:
            node.destroy_node()
