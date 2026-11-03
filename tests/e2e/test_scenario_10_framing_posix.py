"""Scenario 10 framing variant: framing faults cost a bound, then recover.

The proxy corrupts code bytes and delimiters in both directions while the
firmware publishes battery at 50 Hz. A corrupted code byte shifts the
decode and a corrupted delimiter merges two frames, so a touched frame
may share its drop with a neighbour: the MCU's dropped-frames counter is
at most the proxy's host-to-MCU touched count, and the device-to-host seq
gaps are at least its touched user frames. A merge pending when the
faults stop swallows the first clean frame, so the MCU side allows one
drop past its touches and the battery slice extends past the faults until
the first clean arrival proves the boundary resolved. The link stays
connected through the faults, and afterwards the fault-free flow loses
nothing, proving delivery recovers.
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
from e2e_support.fault_proxy import FaultProxy
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: FRM-6, FRM-8, FRM-15, HB-2, HB-7

RATE_HZ = 50
SEED = 20
FRAMING_RATE = 0.01
FAULT_WINDOW_S = 15.0
CLEAN_WINDOW_S = 6.0
# A diagnostics message received this long after the faults stop carries only
# post-fault heartbeats, given the 500 ms heartbeat and 1 Hz diagnostics.
SETTLE_S = 3.0
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = ["battery", "cmd_vel", "echo_vel", "log_text", "time_now"]
TOPIC_BUDGET = 3.0
BATTERY_TOPICS = ["/battery"]
# A merged blob carries up to two touched frames into one drop, so the
# flow floor stays at half the nominal flow: delivery continued through
# the faults even with merges.
FLOW_FLOOR = int(RATE_HZ * FAULT_WINDOW_S * 0.5)
# DDS-side best-effort losses on top of the wire gaps, scenario 2 style.
ROS_SLACK = 5
CLEAN_SLACK = 2
# A touched frame escapes its drop only when the shifted decode still
# passes the CRC, so the gap bound below holds up to stray escapes.
GAP_SLACK = 2

_SCRATCH = None
_FIRMWARE = None
_PROXY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _PROXY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-10f-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = firmware.from_run_script() if "R2S_E2E_FW" in os.environ else firmware.build(scratch)
    _PROXY = FaultProxy(seed=SEED)
    _PROXY.start()
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_PROXY.host_port, _PARAMS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _PROXY is not None:
        _PROXY.stop()
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


def _user_gaps(values):
    """Seq gaps summed over device-to-host topics, whatever is announced."""
    total = 0
    for key, value in values.items():
        if key.startswith("topic ") and key.endswith(" seq gaps"):
            name = key[len("topic ") : -len(" seq gaps")]
            if values.get("topic " + name + " direction") == "device to host":
                total += int(value)
    return total


class TestScenario10Framing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_framing_faults_cost_a_bound_then_recover(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s10_probe")
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
            _SPAWNED = firmware.spawn(_FIRMWARE, _PROXY.mcu_port, ("--rate", str(RATE_HZ)))
            # /diagnostics publishes at 1 Hz, so the observed time can lag the
            # real handshake by one period; the bound still holds because the
            # handshake traffic itself is tens of milliseconds.
            seen = len(received)
            while True:
                message = _fresh_message(node, received, seen)
                seen = len(received)
                missing = checks.handshake_missing(message, HANDSHAKE_TOPICS)
                took = time.monotonic() - _SPAWNED.ready_at
                if not missing:
                    break
                self.assertLess(took, TOPIC_BUDGET, f"handshake still missing {missing} after {took:.1f} s")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
            # The battery flow needs its endpoint matched: the bridge's
            # endpoints appear on the graph once the handshake above completes,
            # so this waits out DDS discovery instead of spending the flow
            # budget on it.
            bridge_support.wait_for_topics(
                node, BATTERY_TOPICS, _SPAWNED.ready_at, checks.DISCOVERY_TIMEOUT, "battery topic"
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
            base_counts = _PROXY.counts()
            base_values = _connected_values(_fresh_message(node, received, seen))
            seen = len(received)
            fault_start = len(battery)
            _PROXY.set_faults(framing_rate=FRAMING_RATE, seed=SEED)
            window_end = time.monotonic() + FAULT_WINDOW_S
            while time.monotonic() < window_end:
                rclpy.spin_once(node, timeout_sec=0.5)
                for message in received[seen:]:
                    self.assertEqual(message.status[0].message, "connected", "link left connected state")
                seen = len(received)
            _PROXY.set_faults()
            # The first clean frame after a pending merge is lost with it,
            # so the slice ends at the first arrival past the faults: from
            # there on every frame proves the boundary resolved.
            straggler_end = len(battery)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while len(battery) == straggler_end:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no battery flow after the faults stopped")
            fault_end = len(battery)
            disabled_at = time.monotonic()
            settled = None
            clean_end = disabled_at + CLEAN_WINDOW_S
            while time.monotonic() < clean_end:
                rclpy.spin_once(node, timeout_sec=0.5)
                if settled is None and time.monotonic() - disabled_at >= SETTLE_S:
                    settled = _connected_values(_fresh_message(node, received, seen))
                    seen = len(received)
            final = _connected_values(_fresh_message(node, received, seen))
            fault_counts = _PROXY.counts()
            self.assertIsNone(_SPAWNED.proc.poll(), "firmware died under framing faults")
            self.assertEqual(final.get("resets"), "0", "MCU rebooted mid-scenario")
            mcu_dropped = int(final["mcu dropped frames"]) - int(base_values["mcu dropped frames"])
            host_touched = fault_counts["host_to_mcu"].frames_touched - base_counts["host_to_mcu"].frames_touched
            # Every drop consumes a touched frame and clean blobs never drop,
            # while a merge carries two touched frames into a single drop.
            # One past the touches is the merge pending at the boundary,
            # which swallows the first clean frame after the faults stop.
            self.assertLessEqual(mcu_dropped, host_touched + 1, "MCU dropped more than the injected faults")
            if host_touched:
                self.assertGreater(mcu_dropped, 0, "touched frames reached the MCU without a drop")
            user_gaps = _user_gaps(final) - _user_gaps(base_values)
            mcu_touched = (
                fault_counts["mcu_to_host"].user_frames_touched - base_counts["mcu_to_host"].user_frames_touched
            )
            self.assertGreater(mcu_touched, 0, "framing faults never touched a user frame")
            # Every touched user frame is lost, and a merge also loses the
            # clean neighbour it swallows, so gaps only exceed the touches.
            self.assertGreaterEqual(user_gaps, 1, "framing faults lost no user frame")
            self.assertGreaterEqual(user_gaps + GAP_SLACK, mcu_touched, "seq gaps below the injected faults")
            battery_gaps = int(final["topic battery seq gaps"]) - int(base_values["topic battery seq gaps"])
            fault_values = battery[fault_start:fault_end]
            checks.check_float_sequence(fault_values, FLOW_FLOOR, battery_gaps + ROS_SLACK, "battery under faults")
            span = int(fault_values[-1]) - int(fault_values[0]) + 1
            self.assertGreaterEqual(
                span - len(fault_values), battery_gaps, "stream lost fewer frames than the counters report"
            )
            clean_values = battery[fault_end:]
            self.assertGreater(len(clean_values), RATE_HZ * CLEAN_WINDOW_S * 0.5, "clean flow stalled")
            checks.check_float_sequence(clean_values, len(clean_values), CLEAN_SLACK, "battery after faults")
            self.assertEqual(
                int(final["mcu dropped frames"]) - int(settled["mcu dropped frames"]),
                0,
                "drops after the faults stopped",
            )
            self.assertEqual(_user_gaps(final) - _user_gaps(settled), 0, "gaps after the faults stopped")
        finally:
            node.destroy_node()
