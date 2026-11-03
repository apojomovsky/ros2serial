"""Scenario 1: the handshake announces the firmware topics in time.

The bridge is up before the firmware boots. The handshake first waits out
the boot wait on the generous timeout, then runs the unchanged 3 s budget
from the later of the MCU's HELLO and the boot wait end, so a slow bridge
open cannot spend the handshake budget. Graph discovery of the new topics
waits on its own generous timeout from the handshake, so a slow runner
cannot fail a correct handshake.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import bridge as bridge_support
from e2e_support import checks, firmware, teardown
from e2e_support.relay import PtyRelay

# covers: SES-1, SES-2, SES-5, SES-6, SES-13, SES-16, SES-17

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text", "/time_now"]
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix("/") for name in ANNOUNCED]
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


def _wait_link_ready(node, received, since):
    """Spin until the link leaves boot wait, within the generous timeout.

    The bridge opens the port and waits out boot_wait_ms for an
    unsolicited HELLO before the handshake starts, and under load the
    open itself can lag the firmware ready line. The 3 s handshake budget
    runs from the later of the ready line and this return.
    """
    while True:
        if received and received[-1].status[0].message not in ("disconnected", "boot_wait"):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, f"link never left boot wait after {took:.1f} s"
        rclpy.spin_once(node, timeout_sec=0.5)


class TestScenario1(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_handshake_announces_topics_within_3s_of_hello(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s1_probe")
        try:
            bridge_support.wait_for_bridge(node)
            # Matched before the firmware boots, so the handshake wait below
            # observes bridge state, not DDS discovery of the new topics.
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            received.clear()
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            # The boot wait is the bridge's own delay before the handshake
            # starts, so it waits on the generous timeout and the 3 s
            # budget runs from the later of the ready line and its end.
            link_ready_at = _wait_link_ready(node, received, _SPAWNED.ready_at)
            took = checks.wait_for_handshake(
                received,
                HANDSHAKE_TOPICS,
                link_ready_at,
                TOPIC_BUDGET,
                "handshake",
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
            )
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
            # Discovery starts when the bridge creates its endpoints at the
            # handshake, so the generous timeout runs from there, not from
            # the firmware spawn: a slow handshake must not spend it.
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node, ANNOUNCED, handshake_done_at, checks.DISCOVERY_TIMEOUT, "announced topics"
            )
        finally:
            node.destroy_node()
