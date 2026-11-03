"""Soak on simavr: a wall-clock window of clean-link traffic on the Uno.

The slice sketch publishes battery at 50 Hz of simulated time for
R2S_SOAK_SECONDS of wall time while the test samples /diagnostics and the
bridge RSS. Simulated time runs off the wall clock, so the message floor
only proves sustained flow and the exactness lives in the wire counters,
which must read identical before and after. The battery stream stays
integral and ordered within the scenario 2 loss precedent, and the bridge
RSS plateaus after warm-up. Run through tests/e2e/run-soak.sh, which builds
every binary and raises the harness simulated budget past the window.
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
from e2e_support import soak as soak_gate
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

RATE_HZ = 50
DURATION_S = float(os.environ.get("R2S_SOAK_SECONDS", "7200"))
# Allocator arenas settle early; the first tenth of the window, capped at ten
# minutes, never counts toward growth, so short local runs keep a warm-up too.
WARMUP_S = min(600.0, DURATION_S / 10.0)
RSS_EVERY_S = 10.0
# Simulated time runs off the wall clock, so this floor only proves the flow
# never stalled: 5% of nominal, enough for minutes of simulated publishes.
COUNT_FLOOR = max(50, int(0.05 * RATE_HZ * DURATION_S))
# A five minute silence is a stall on either clock.
GAP_BUDGET_S = 300.0
# A leak of one frame per message would grow megabytes per hour; five
# megabytes after warm-up catches it with headroom for allocator noise.
MEM_TOL_KB = 5120.0
HANDSHAKE_TOPICS = ["battery", "cmd_vel", "echo_vel", "log_text"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-soak-avr-")
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


def _fresh_values(node, received, seen):
    deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
    while len(received) <= seen:
        rclpy.spin_once(node, timeout_sec=0.5)
        assert time.monotonic() < deadline, "no fresh /diagnostics from the bridge"
    return {value.key: value.value for value in received[-1].status[0].values}


class TestSoak(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_clean_link_flows_without_drift_or_growth(self):
        node = rclpy.create_node("e2e_soak_avr_probe")
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            seen = len(received)
            start_handshake = time.monotonic()
            while True:
                values = _fresh_values(node, received, seen)
                seen = len(received)
                missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS)
                took = time.monotonic() - start_handshake
                if not missing:
                    break
                self.assertLess(took, TOPIC_BUDGET, f"handshake still missing {missing} after {took:.1f} s")
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            flow = []
            node.create_subscription(
                Float32, "battery", lambda msg: flow.append((time.monotonic(), msg.data)), best_effort
            )
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not flow:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            base = dict(values)
            start = time.monotonic()
            bridge_samples = []
            connected = 0
            next_rss = start
            while time.monotonic() - start < DURATION_S:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertIsNone(_SPAWNED.proc.poll(), "harness died mid-soak")
                for message in received[seen:]:
                    self.assertEqual(message.status[0].message, "connected", "link left connected state")
                    connected += 1
                seen = len(received)
                now = time.monotonic()
                if now >= next_rss:
                    next_rss = now + RSS_EVERY_S
                    pids = soak_gate.find_bridge_pids()
                    self.assertEqual(len(pids), 1, f"want one bridge below this test, found {pids}")
                    bridge_samples.append((now - start, soak_gate.rss_kb(pids[0])))
            final = _fresh_values(node, received, seen)
            self.assertGreater(connected, 0, "no /diagnostics during the window")
            soak_gate.check_wire_counters(base, final)
            stamps = [stamp for stamp, _ in flow]
            self.assertLessEqual(
                max([b - a for a, b in zip(stamps, stamps[1:], strict=False)] + [0.0]),
                GAP_BUDGET_S,
                "battery flow stalled mid-soak",
            )
            flow_values = [value for _, value in flow]
            span = int(flow_values[-1]) - int(flow_values[0]) + 1 if flow_values else 0
            checks.check_float_sequence(flow_values, COUNT_FLOOR, max(5, int(0.01 * span)), "battery soak")
            soak_gate.check_memory_growth(bridge_samples, WARMUP_S, MEM_TOL_KB, "bridge RSS")
            self.assertEqual(_SPAWNED.stop(), 0, "the harness stopped dirty")
        finally:
            node.destroy_node()
