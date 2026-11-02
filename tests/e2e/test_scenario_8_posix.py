"""Scenario 8: time sync end to end with a skewed MCU clock.

The firmware ticks 5000 ppm fast. After the initial burst the bridge's
corrections anchor the MCU clock, and the time_now stamps stay within
R2S_SYNC_BOUND_NS nanoseconds (default 1000000) of host time. A fast
clock needs negative drift: its ticks outrun ROS time, so each nominal
millisecond carries fewer ROS nanoseconds. The stamps are only judged
once the fitted drift tracks the skew.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from builtin_interfaces.msg import Time
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import bridge as bridge_support
from e2e_support import checks, firmware
from e2e_support.relay import PtyRelay
from rclpy.qos import QoSProfile, ReliabilityPolicy

# covers: SES-22, SES-23, SES-24, SES-25

SKEW_PPM = 5000


def _sync_bound_ns():
    """The median bound from R2S_SYNC_BOUND_NS, defaulting to 1 ms."""
    raw = os.environ.get("R2S_SYNC_BOUND_NS", "1000000")
    try:
        return int(raw)
    except ValueError as err:
        raise ValueError(f"R2S_SYNC_BOUND_NS must be an integer number of nanoseconds, got {raw!r}") from err


BOUND_NS = _sync_bound_ns()
COLLECT_S = 10.0
MIN_STAMPS = 50
SYNC_TIMEOUT = 30.0
SETTLE_TIMEOUT = 120.0
# A 5000 ppm fast clock reads (1 / 1.005 - 1) of that in ROS nanoseconds
# per nominal millisecond, so the settled drift sits near -4975 ppm. The
# gate proves the fit converged and the knob took effect: ten readings
# over ten seconds must agree and sit near the known skew. The median
# below judges the accuracy; the gate must not pin the estimator's exact
# fitted value.
DRIFT_TOL = 100
DRIFT_COUNT = 10
DRIFT_SAMPLE_DT = 1.1
DRIFT_BAND = 1000
# A starved MCU loop delays PINGs but answers PONGs at once, biasing
# every sample one way; the estimator cannot see it. The test waits for
# a responsive link before judging: a fast round trip proves it. The
# wait is bounded and never fails: the median below still judges.
QUIET_TIMEOUT = 60.0
QUIET_RTT_NS = 2000000
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = ["battery", "cmd_vel", "echo_vel", "log_text", "time_now"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-8-")
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


def _diagnostic_values(received):
    values = received[-1].status[0].values
    return {value.key: value.value for value in values}


class TestScenario8(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_skewed_mcu_time_stays_within_1ms_of_host_time(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s8_probe")
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
            # The bridge publishes MCU topics best effort, so the probe
            # subscribes best effort too; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            stamps = []
            node.create_subscription(
                Time,
                "time_now",
                lambda msg: stamps.append((msg.sec * 1_000_000_000 + msg.nanosec, node.get_clock().now().nanoseconds)),
                best_effort,
            )
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10", "--tick-skew-ppm", str(SKEW_PPM)))
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
            deadline = time.monotonic() + SYNC_TIMEOUT
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                sync = _diagnostic_values(received)
                if sync.get("time sync offset") != "not available":
                    break
                self.assertLess(time.monotonic(), deadline, "time sync never became available")
            deadline = time.monotonic() + SETTLE_TIMEOUT
            history = []
            rtts = []
            last_sample = 0.0
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "drift never settled near the known skew")
                if time.monotonic() - last_sample < DRIFT_SAMPLE_DT:
                    continue
                last_sample = time.monotonic()
                sync = _diagnostic_values(received)
                drift = sync.get("time sync drift")
                if drift is None or not drift.lstrip("-").isdigit():
                    continue
                history.append(int(drift))
                history = history[-DRIFT_COUNT:]
                rtt = sync.get("time sync round trip")
                if rtt is not None and rtt.isdigit():
                    rtts.append(int(rtt))
                if (
                    len(history) == DRIFT_COUNT
                    and max(history) - min(history) <= DRIFT_TOL
                    and abs(sum(history) // DRIFT_COUNT + SKEW_PPM) <= DRIFT_BAND
                ):
                    break
            quiet_deadline = time.monotonic() + QUIET_TIMEOUT
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                rtt = _diagnostic_values(received).get("time sync round trip")
                if rtt is not None and rtt.isdigit() and int(rtt) < QUIET_RTT_NS:
                    break
                if time.monotonic() >= quiet_deadline:
                    break
            # The link delay rides on every stamp, so the test subtracts
            # the fastest round trip the bridge measured, halved: the
            # estimator fits the offset from the lowest round trips, so the
            # correction uses its statistic, and a scheduling spike delays
            # a trip without moving the clock. The median judges the clock:
            # a correction the MCU ignored leaves the stamps adrift by
            # milliseconds within seconds, median included.
            stamps.clear()
            window_end = time.monotonic() + COLLECT_S
            while time.monotonic() < window_end:
                # Short spins keep each callback near its arrival: a long
                # spin would queue several stamps behind one wakeup and read
                # the clock late for all but the first.
                rclpy.spin_once(node, timeout_sec=0.05)
                rtt = _diagnostic_values(received).get("time sync round trip")
                if rtt is not None and rtt.isdigit():
                    rtts.append(int(rtt))
            self.assertGreaterEqual(len(stamps), MIN_STAMPS, "too few time_now stamps arrived")
            self.assertGreaterEqual(len(rtts), 5, "too few round trips came back")
            one_way_ns = min(rtts) // 2
            errors = sorted(abs(stamp_ns - (host_ns - one_way_ns)) for stamp_ns, host_ns in stamps)
            self.assertLessEqual(errors[len(errors) // 2], BOUND_NS, f"the median stamp left the {BOUND_NS} ns bound")
        finally:
            node.destroy_node()
