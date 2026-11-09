"""Scenario 12: the simavr MCU publishes more than the link carries.

The flood firmware publishes battery samples through a per-tick byte
budget in the test-only port hook, so a publish that finds the budget dry
returns TX_BUSY and its sample never takes a seq. The host stream stays
integral and ordered, with gaps where samples were shed, and the bridge
seq gaps stay 0 while the firmware busy count climbs: the drops are
counted where they happen, and no frame on the wire is corrupted by them.
The flood sketch announces the same topics as the slice, so scenarios 1
to 11 run unchanged on their own firmware.
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
from e2e_support import checks
from e2e_support import teardown
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from std_msgs.msg import Float32
from std_msgs.msg import String

# covers: ERR-3, ERR-6, FRM-5, FRM-15

# Count based gates: simavr runs unpaced, so the same publishes take a
# wall time the host load decides. The flood moves thousands of samples;
# this floor only proves the flow is alive, since shed samples are the
# point of the scenario.
RECEIVE_COUNT = 500
FLOW_TIMEOUT = 120.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-avr-12-')
    _SPAWNED = avr_support.spawn(os.environ.get('R2S_E2E_BOARD', 'uno'), flavor='flood')
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


def _parse_busy(text):
    """The flood report's busy count, 8 hex digits like the slice reports."""
    return int(text.strip(), 16)


class TestScenario12(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_saturation_sheds_samples_with_busy_and_whole_frames(self):
        node = rclpy.create_node('e2s_s12_avr_probe')
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            # The bridge publishes MCU topics best effort, so the probe
            # subscribes best effort too; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            values = []
            node.create_subscription(
                Float32, 'battery', lambda msg: values.append(msg.data), best_effort
            )
            reports = []
            node.create_subscription(
                String, 'log_text', lambda msg: reports.append(msg.data), best_effort
            )
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not values:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no Float32 flow on battery')
            checks.collect_until(
                values,
                RECEIVE_COUNT,
                time.monotonic() + FLOW_TIMEOUT,
                'battery flood',
                lambda: rclpy.spin_once(node, timeout_sec=0.5),
            )
            missing = checks.handshake_missing(received[-1], ['battery'])
            self.assertFalse(missing, f'link not connected: {missing}')
            for value in values:
                self.assertTrue(float(value).is_integer(), f'corrupt value {value!r}')
            for earlier, later in zip(values, values[1:], strict=False):
                self.assertGreater(
                    later, earlier, f'values out of order: {later!r} after {earlier!r}'
                )
            span = int(values[-1]) - int(values[0]) + 1
            self.assertGreater(span - len(values), 0, 'no sample was shed on a saturated link')
            deadline = time.monotonic() + FLOW_TIMEOUT
            busy = 0
            while busy == 0:
                self.assertLess(time.monotonic(), deadline, 'the firmware reported no busy count')
                rclpy.spin_once(node, timeout_sec=0.5)
                for report in reports:
                    busy = max(busy, _parse_busy(report))
            gaps = {value.key: value.value for value in received[-1].status[0].values}
            self.assertEqual(
                gaps.get(checks.topic_key(gaps, 'battery', 'seq gaps')),
                '0',
                'shed samples took a seq',
            )

            # Battery is the first announced topic, so its diagnostics id is 16.
            # Reports are rate limited to one per second, so the test waits
            # for the running sum instead of assuming the first sample has it.
            def latest():
                return {v.key: v.value for v in received[-1].status[0].values}

            deadline = time.monotonic() + FLOW_TIMEOUT
            while int(latest().get('mcu error code 6', '0')) == 0:
                self.assertLess(time.monotonic(), deadline, 'busy sum never reached /diagnostics')
                rclpy.spin_once(node, timeout_sec=0.5)
            breakdown = latest().get('topic 16 battery mcu errors', '')
            slots = dict(part.split('=') for part in breakdown.split())
            self.assertGreater(
                int(slots.get('6', '0')), 0, 'battery breakdown misses the busy count'
            )
            self.assertEqual(_SPAWNED.stop(), 0, 'the harness stopped dirty')
        finally:
            node.destroy_node()
