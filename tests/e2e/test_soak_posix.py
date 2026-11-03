"""Soak on posix: two hours of clean-link traffic against the virtual MCU.

The firmware publishes battery at 50 Hz for R2S_SOAK_SECONDS while the test
samples /diagnostics and both processes' RSS. The wire counters must read
identical before and after, since a clean link drops nothing; the battery
stream stays integral and ordered within the scenario 2 loss precedent for
the DDS hop; bridge and firmware RSS plateau after warm-up. Run through
tests/e2e/run-soak.sh, which builds every binary and sets the window.
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
from e2e_support import soak as soak_gate
from e2e_support.relay import PtyRelay
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

RATE_HZ = 50
DURATION_S = float(os.environ.get("R2S_SOAK_SECONDS", "7200"))
# Allocator arenas settle early; the first tenth of the window, capped at ten
# minutes, never counts toward growth, so short local runs keep a warm-up too.
WARMUP_S = min(600.0, DURATION_S / 10.0)
RSS_EVERY_S = 10.0
# The scenario 2 precedent for the best-effort DDS hop, applied to the
# observed span below: 1% of what the MCU sent. The wire counters stay
# exactly consistent.
COUNT_FLOOR = int(0.9 * RATE_HZ * DURATION_S)
# A five minute silence on a 50 Hz stream is a stall, whatever the host load.
GAP_BUDGET_S = 300.0
# A leak of one frame per message would grow megabytes per hour; five
# megabytes after warm-up catches it with headroom for allocator noise.
MEM_TOL_KB = 5120.0
HANDSHAKE_TOPICS = ["battery", "cmd_vel", "echo_vel", "log_text", "time_now"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-soak-")
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
        global _SPAWNED
        node = rclpy.create_node("e2e_soak_probe")
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", str(RATE_HZ)))
            seen = len(received)
            while True:
                values = _fresh_values(node, received, seen)
                seen = len(received)
                missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS)
                took = time.monotonic() - _SPAWNED.ready_at
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
            firmware_samples = []
            connected = 0
            next_rss = start
            while time.monotonic() - start < DURATION_S:
                rclpy.spin_once(node, timeout_sec=0.5)
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
                    firmware_samples.append((now - start, soak_gate.rss_kb(_SPAWNED.proc.pid)))
            final = _fresh_values(node, received, seen)
            self.assertGreater(connected, 0, "no /diagnostics during the window")
            self.assertIsNone(_SPAWNED.proc.poll(), "firmware died mid-soak")
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
            soak_gate.check_memory_growth(firmware_samples, WARMUP_S, MEM_TOL_KB, "firmware RSS")
        finally:
            node.destroy_node()
