"""Scenario 10: 1% byte corruption costs exactly the touched frames.

The proxy corrupts both directions while the firmware publishes battery at
50 Hz. Every touched frame is dropped exactly once by its receiver, so the
MCU's dropped-frames counter matches the proxy's host-to-MCU touched count
and the device-to-host seq gaps match its touched user frames. Afterwards
the fault-free flow loses nothing, proving delivery recovers. The initial
handshake first waits out the boot wait on the generous timeout, then runs
the unchanged 3 s budget from the later of the ready line and the boot
wait end, so a slow bridge open cannot spend the handshake budget.
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
from e2e_support import checks
from e2e_support import firmware
from e2e_support import teardown
from e2e_support.fault_proxy import FaultProxy
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from std_msgs.msg import Float32

# covers: FRM-8, FRM-15, HB-2, HB-7

RATE_HZ = 50
SEED = 10
CORRUPT_RATE = 0.01
FAULT_WINDOW_S = 15.0
CLEAN_WINDOW_S = 6.0
# A diagnostics message received this long after the faults stop carries only
# post-fault heartbeats, given the 500 ms heartbeat and 1 Hz diagnostics.
SETTLE_S = 3.0
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = ['battery', 'cmd_vel', 'echo_vel', 'log_text', 'time_now']
TOPIC_BUDGET = 3.0
BATTERY_TOPICS = ['/battery']
# About one frame in thirteen carries a corrupted byte at this rate, so half
# the nominal flow still arriving proves delivery continued through faults.
FLOW_FLOOR = int(RATE_HZ * FAULT_WINDOW_S * 0.5)
# DDS-side best-effort losses on top of the wire gaps, scenario 2 style.
ROS_SLACK = 5
CLEAN_SLACK = 2
# In-flight frames land within a fraction of this after the faults stop,
# so the loss slice ending past the drain holds every boundary gap.
DRAIN_S = 1.0
# Frames in flight when the faults turn on straddle the enable: published
# clean but dropped faulty. The slice starts this many arrivals early so
# it holds them; earlier flow is fault-free, so the cover arrivals are
# all present and leave the span count exact.
HEAD_COVER = 10

_SCRATCH = None
_FIRMWARE = None
_PROXY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _PROXY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-10-')
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = (
        firmware.from_run_script() if 'R2S_E2E_FW' in os.environ else firmware.build(scratch)
    )
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
        assert time.monotonic() < deadline, 'no fresh /diagnostics from the bridge'
    return received[-1]


def _connected_values(message):
    """Key/value pairs of a /diagnostics message on a connected link."""
    assert message.status[0].message == 'connected', 'link left connected state'
    return {value.key: value.value for value in message.status[0].values}


def _user_gaps(values):
    """Seq gaps summed over device-to-host topics, whatever is announced."""
    total = 0
    for key, value in values.items():
        if key.startswith('topic ') and key.endswith(' seq gaps'):
            middle = key[len('topic ') : -len(' seq gaps')]
            if values.get('topic ' + middle + ' direction') == 'device to host':
                total += int(value)
    return total


def _wait_link_ready(node, received, seen, since):
    """Spin until the link leaves boot wait, within the generous timeout.

    The bridge opens the port and waits out boot_wait_ms for an
    unsolicited HELLO before the handshake starts, and under load the
    open itself can lag the firmware ready line. The 3 s handshake budget
    runs from the later of the ready line and this return, so the fresh
    report count travels with it.
    """
    while True:
        message = _fresh_message(node, received, seen)
        seen = len(received)
        if message.status[0].message not in ('disconnected', 'boot_wait'):
            return max(since, time.monotonic()), seen
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, (
            f'link never left boot wait after {took:.1f} s'
        )


class TestScenario10(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_corruption_costs_exactly_the_touched_frames_then_recovers(self):
        global _SPAWNED
        node = rclpy.create_node('e2e_s10_probe')
        try:
            bridge_support.wait_for_bridge(node)
            # Matched before the firmware boots, so the handshake wait below
            # observes bridge state, not DDS discovery of the new topics.
            received = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            _SPAWNED = firmware.spawn(_FIRMWARE, _PROXY.mcu_port, ('--rate', str(RATE_HZ)))
            # The boot wait is the bridge's own delay before the handshake
            # starts, so it waits on the generous timeout and the 3 s
            # budget runs from the later of the ready line and its end.
            seen = len(received)
            link_ready_at, seen = _wait_link_ready(node, received, seen, _SPAWNED.ready_at)
            took = checks.wait_for_handshake(
                received,
                HANDSHAKE_TOPICS,
                link_ready_at,
                TOPIC_BUDGET,
                'handshake',
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
                clock=time.monotonic,
            )
            seen = len(received)
            self.assertLessEqual(took, TOPIC_BUDGET, 'handshake took longer than 3 s')
            # The battery flow needs its endpoint matched: the bridge's
            # endpoints appear on the graph once the handshake above completes,
            # so this waits out DDS discovery instead of spending the flow
            # budget on it. The generous timeout runs from the handshake, not
            # from the firmware spawn: a slow handshake must not spend it.
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node,
                BATTERY_TOPICS,
                handshake_done_at,
                checks.DISCOVERY_TIMEOUT,
                'battery topic',
                lambda: rclpy.spin_once(node, timeout_sec=bridge_support.DISCOVERY_SPIN),
            )
            # The bridge forwards MCU traffic best effort, so the probe
            # matches that; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            battery = []
            node.create_subscription(
                Float32, 'battery', lambda msg: battery.append(msg.data), best_effort
            )
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not battery:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no Float32 flow on battery')
            base_counts = _PROXY.counts()
            base_values = _connected_values(_fresh_message(node, received, seen))
            seen = len(received)
            fault_start = max(0, len(battery) - HEAD_COVER)
            _PROXY.set_faults(corrupt_rate=CORRUPT_RATE, seed=SEED)
            window_end = time.monotonic() + FAULT_WINDOW_S
            while time.monotonic() < window_end:
                rclpy.spin_once(node, timeout_sec=0.5)
            _PROXY.set_faults()
            # A gap counts at the bridge when its revealing frame arrives,
            # which can lag the window end through the proxy, the bridge
            # and DDS. The slice ends past the proxy's first post-fault
            # flush, which the frame counter proves, and past a drain for
            # the lag, so every boundary gap and its revealing frame are
            # inside the slice.
            flushed = _PROXY.counts()['mcu_to_host'].frames
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while _PROXY.counts()['mcu_to_host'].frames <= flushed:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'proxy never flushed past the faults')
            drain_end = time.monotonic() + DRAIN_S
            while time.monotonic() < drain_end:
                rclpy.spin_once(node, timeout_sec=0.5)
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
            self.assertIsNone(_SPAWNED.proc.poll(), 'firmware died under corruption')
            self.assertEqual(final.get('resets'), '0', 'MCU rebooted mid-scenario')
            mcu_dropped = int(final['mcu dropped frames']) - int(base_values['mcu dropped frames'])
            host_touched = (
                fault_counts['host_to_mcu'].frames_touched
                - base_counts['host_to_mcu'].frames_touched
            )
            self.assertEqual(
                mcu_dropped, host_touched, 'MCU drops do not match the injected faults'
            )
            user_gaps = _user_gaps(final) - _user_gaps(base_values)
            mcu_touched = (
                fault_counts['mcu_to_host'].user_frames_touched
                - base_counts['mcu_to_host'].user_frames_touched
            )
            self.assertEqual(user_gaps, mcu_touched, 'seq gaps do not match the injected faults')
            gaps_key = checks.topic_key(final, 'battery', 'seq gaps')
            battery_gaps = int(final[gaps_key]) - int(base_values[gaps_key])
            fault_values = battery[fault_start:fault_end]
            checks.check_float_sequence(
                fault_values, FLOW_FLOOR, battery_gaps + ROS_SLACK, 'battery under faults'
            )
            span = int(fault_values[-1]) - int(fault_values[0]) + 1
            self.assertGreaterEqual(
                span - len(fault_values),
                battery_gaps,
                'stream lost fewer frames than the counters report',
            )
            clean_values = battery[fault_end:]
            self.assertGreater(
                len(clean_values), RATE_HZ * CLEAN_WINDOW_S * 0.5, 'clean flow stalled'
            )
            checks.check_float_sequence(
                clean_values, len(clean_values), CLEAN_SLACK, 'battery after faults'
            )
            self.assertEqual(
                int(final['mcu dropped frames']) - int(settled['mcu dropped frames']),
                0,
                'drops after the faults stopped',
            )
            self.assertEqual(
                _user_gaps(final) - _user_gaps(settled), 0, 'gaps after the faults stopped'
            )
        finally:
            node.destroy_node()
