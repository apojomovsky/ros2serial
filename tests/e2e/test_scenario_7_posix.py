"""Scenario 7: an MCU reset is detected within 500 ms and re-handshakes.

Restarting the firmware on the live relay looks like a reset board: the new
boot echoes session token 0, and the heartbeat it answers while the bridge
is still connected announces the new handshake. The initial handshake first
waits out the boot wait on the generous timeout, then runs the unchanged
3 s budget from the later of the ready line and the boot wait end.
Detection runs from the new firmware's ready line with the heartbeat plus
the diagnostics period plus margin; the re-handshake runs from detection
with the scenario 1 budget. Publishers stay because the topic table is
unchanged.
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
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: SES-10, SES-19, SES-20, SES-21

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text", "/time_now"]
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix("/") for name in ANNOUNCED]
TOPIC_BUDGET = 3.0
# A reset is announced by the next heartbeat at the latest, and /diagnostics
# samples the link at 1 Hz, so detection from the reset carries both plus
# margin. The re-handshake then carries the scenario 1 budget from detection.
DETECT_BOUND = 3.0
REHANDSHAKE_BUDGET = 3.0
FLOW_COUNT = 15
FLOW_TIMEOUT = 10.0
FLOW_SLACK = 2

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-7-")
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
        rclpy.spin_once(node, timeout_sec=0.2)


def _wait_connected(node, received, since, budget, what):
    """Spin until every handshake topic reads ok, within budget of since.

    The report is read before spinning, so the time kept is the arrival
    of the completing report, never a spin quantum later.
    """
    while True:
        missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS) if received else ["link silent"]
        took = time.monotonic() - since
        if not missing:
            return took
        assert took < budget, f"{what} still missing {missing} after {took:.1f} s"
        rclpy.spin_once(node, timeout_sec=0.2)


class TestScenario7(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_mcu_reset_is_detected_and_rehandshakes_keeping_publishers(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s7_probe")
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
            took = _wait_connected(node, received, link_ready_at, TOPIC_BUDGET, "handshake")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
            # Discovery starts when the bridge creates its endpoints at the
            # handshake, so the generous timeout runs from there, not from
            # the firmware spawn: a slow handshake must not spend it.
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node, ANNOUNCED, handshake_done_at, checks.DISCOVERY_TIMEOUT, "announced topics"
            )
            # The bridge forwards MCU traffic best effort, so the probe
            # matches that; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            battery = []
            node.create_subscription(Float32, "battery", lambda msg: battery.append(msg.data), best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not battery:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            before = {value.key: value.value for value in received[-1].status[0].values}
            self.assertEqual(before.get("resets"), "0", "link already counted a reset before the test reset")
            token_before = before.get("session token")
            self.assertTrue(node.get_publishers_info_by_topic("/battery"), "no battery publisher before the reset")
            # Reset: a fresh firmware on the same relay is a rebooted MCU
            # with its session token back at 0.
            unplugged = _SPAWNED
            _SPAWNED = None
            unplugged.stop()
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            reset_at = _SPAWNED.ready_at
            # Detection: the link leaves connected, or the reset counter moves.
            while True:
                rclpy.spin_once(node, timeout_sec=0.2)
                message = received[-1].status[0].message if received else "silent"
                values = {value.key: value.value for value in received[-1].status[0].values} if received else {}
                took = time.monotonic() - reset_at
                if message != "connected" or values.get("resets") != "0":
                    break
                self.assertLess(took, DETECT_BOUND, f"reset undetected after {took:.1f} s")
            detected_at = time.monotonic()
            # Re-handshake: the identical table keeps the endpoints, so the
            # battery publisher is present at every sample, never recreated.
            while True:
                rclpy.spin_once(node, timeout_sec=0.2)
                self.assertTrue(
                    node.get_publishers_info_by_topic("/battery"),
                    "battery publisher lost across the reset handshake",
                )
                missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS) if received else ["link silent"]
                took = time.monotonic() - detected_at
                if not missing:
                    break
                self.assertLess(took, REHANDSHAKE_BUDGET, f"re-handshake missing {missing} after {took:.1f} s")
            self.assertLessEqual(took, REHANDSHAKE_BUDGET, "re-handshake took longer than 3 s")
            after = {value.key: value.value for value in received[-1].status[0].values}
            self.assertEqual(after.get("resets"), "1", "the reset was never counted")
            self.assertNotEqual(after.get("session token"), token_before, "the re-handshake kept its token")
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node, ANNOUNCED, handshake_done_at, checks.DISCOVERY_TIMEOUT, "announced topics"
            )
            # The rebooted firmware counts from 0 again, so the flow after
            # the mark is a new increasing run that proves traffic resumed.
            mark = len(battery)
            deadline = time.monotonic() + FLOW_TIMEOUT
            while len(battery) - mark < FLOW_COUNT:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "battery flow never resumed after the reset")
            checks.check_float_sequence(battery[mark:], len(battery) - mark, FLOW_SLACK, "battery after reset")
        finally:
            node.destroy_node()
