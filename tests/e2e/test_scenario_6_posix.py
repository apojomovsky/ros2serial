"""Scenario 6: unplug (close the pty) and replug without restarting the bridge.

The bridge opens a stable symlink to the relay slave. Unplug stops the
relay and the firmware and removes the link, so the bridge reads EIO,
enters DISCONNECTED, and its reopen attempts fail. Replug brings a new
relay, remakes the link, and boots a fresh firmware. The bound covers a
reopen retry plus the boot wait plus the handshake, measured from the new
firmware's ready line, never from the spawn. The initial handshake first
waits out the boot wait on the generous timeout, then runs the unchanged
3 s budget from the later of the ready line and the boot wait end, so a
slow bridge open cannot spend the handshake budget.
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
from rcl_interfaces.srv import GetParameters
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: SES-12, SES-13, SES-21

ANNOUNCED = ["/battery", "/cmd_vel", "/echo_vel", "/log_text", "/time_now"]
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix("/") for name in ANNOUNCED]
TOPIC_BUDGET = 3.0
FLOW_COUNT = 15
FLOW_TIMEOUT = 10.0
FLOW_SLACK = 2
# The unplug is already proven once the bridge reports disconnected; the
# hold keeps it down through a few failed reopen attempts, so the retry loop
# proves it never gives up or exits.
UNPLUG_HOLD_S = 3.0
DISCONNECTED_TIMEOUT = 10.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_LINK = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _LINK, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-6-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = firmware.from_run_script() if "R2S_E2E_FW" in os.environ else firmware.build(scratch)
    _RELAY = PtyRelay()
    _RELAY.start()
    _LINK = scratch / "bridge_port"
    os.symlink(_RELAY.host_port, _LINK)
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(str(_LINK), _PARAMS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _RELAY is not None:
        try:
            _RELAY.stop()
        except OSError:
            # The test already stopped this relay at unplug time.
            pass
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


def _fresh_message(node, received, seen):
    """Spin until a new /diagnostics message lands and return it."""
    deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
    while len(received) <= seen:
        rclpy.spin_once(node, timeout_sec=0.5)
        assert time.monotonic() < deadline, "no fresh /diagnostics from the bridge"
    return received[-1]


def _connected_values(message):
    """Key/value pairs of a /diagnostics message on a connected link."""
    assert message.status[0].message == "connected", "link left connected state"
    return {value.key: value.value for value in message.status[0].values}


def _wait_link_ready(node, received, since):
    """Spin until the link leaves boot wait, within the generous timeout.

    The bridge opens the port and waits out boot_wait_ms for an
    unsolicited HELLO before the handshake starts, and under load the
    open itself can lag the firmware ready line. The 3 s handshake budget
    runs from the later of the ready line and this return.
    """
    while True:
        rclpy.spin_once(node, timeout_sec=0.5)
        if received and received[-1].status[0].message not in ("disconnected", "boot_wait"):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, f"link never left boot wait after {took:.1f} s"


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
        rclpy.spin_once(node, timeout_sec=0.5)


def _boot_wait_s(node):
    """The bridge's boot_wait_ms parameter in seconds, read off the node.

    The reconnect bound carries the boot wait on top of the handshake budget,
    so it tracks the params file instead of hardcoding it.
    """
    client = node.create_client(GetParameters, "/r2s_bridge/get_parameters")
    try:
        assert client.wait_for_service(timeout_sec=bridge_support.HANDSHAKE_TIMEOUT), "no bridge parameter service"
        future = client.call_async(GetParameters.Request(names=["boot_wait_ms"]))
        rclpy.spin_until_future_complete(node, future, timeout_sec=bridge_support.HANDSHAKE_TIMEOUT)
        assert future.done(), "bridge parameter call timed out"
        values = future.result().values
        assert len(values) == 1, "bridge returned no boot_wait_ms"
        return values[0].integer_value / 1000.0
    finally:
        node.destroy_client(client)


class TestScenario6(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_unplug_and_replug_reconnects_without_restarting_the_bridge(self):
        global _SPAWNED, _RELAY
        node = rclpy.create_node("e2e_s6_probe")
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
            # Unplug: cut both directions at once, stop the firmware, and
            # remove the link, so the bridge reads a hangup and its reopen
            # attempts fail instead of landing on a dead slave.
            unplugged = _SPAWNED
            _SPAWNED = None
            _RELAY.stop()
            unplugged.stop()
            os.unlink(_LINK)
            deadline = time.monotonic() + DISCONNECTED_TIMEOUT
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "bridge never reported disconnected after unplug")
                if received and received[-1].status[0].message == "disconnected":
                    break
            hold_end = time.monotonic() + UNPLUG_HOLD_S
            while time.monotonic() < hold_end:
                rclpy.spin_once(node, timeout_sec=0.5)
            # Replug: a new relay under the same link, then a fresh firmware.
            _RELAY = PtyRelay()
            _RELAY.start()
            os.symlink(_RELAY.host_port, _LINK)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            reconnect_budget = _boot_wait_s(node) + TOPIC_BUDGET
            took = _wait_connected(node, received, _SPAWNED.ready_at, reconnect_budget, "reconnect")
            self.assertLessEqual(took, reconnect_budget, "reconnect took longer than boot wait plus 3 s")
            final = _connected_values(received[-1])
            self.assertEqual(final.get("resets"), "0", "a clean replug is not an MCU reset")
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node, ANNOUNCED, handshake_done_at, checks.DISCOVERY_TIMEOUT, "announced topics"
            )
            # The fresh firmware counts from 0 again, so the flow after the
            # mark is a new increasing run that proves traffic resumed.
            mark = len(battery)
            deadline = time.monotonic() + FLOW_TIMEOUT
            while len(battery) - mark < FLOW_COUNT:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "battery flow never resumed after replug")
            checks.check_float_sequence(battery[mark:], len(battery) - mark, FLOW_SLACK, "battery after replug")
        finally:
            node.destroy_node()
