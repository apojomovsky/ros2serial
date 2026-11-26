# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 20: qos.overrides lets a reliable subscriber receive battery.

The MCU publishes battery best effort, so a default rclpy subscriber
(reliable) would never match the bridge's best-effort publisher. With
qos.overrides naming battery reliable, the host publisher is reliable
and the same default subscriber receives the flow.
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
from e2e_support.relay import PtyRelay
from std_msgs.msg import Float32

# covers: SES-31

HANDSHAKE_TOPICS = ['battery', 'cmd_vel', 'echo_vel', 'log_text', 'time_now']
TOPIC_BUDGET = 3.0
FLOW_TIMEOUT = 20.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-20-')
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = (
        firmware.from_run_script() if 'R2S_E2E_FW' in os.environ else firmware.build(scratch)
    )
    _RELAY = PtyRelay()
    _RELAY.start()
    _PARAMS = scratch / 'qos_params.yaml'
    _PARAMS.write_text(
        'r2s_bridge:\n  ros__parameters:\n    qos:\n      overrides: ["battery=reliable"]\n',
        encoding='utf-8',
    )


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
    """Spin until the link leaves boot wait, within the generous timeout."""
    while True:
        if received and received[-1].status[0].message not in ('disconnected', 'boot_wait'):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, (
            f'link never left boot wait after {took:.1f} s'
        )
        rclpy.spin_once(node, timeout_sec=0.5)


class TestScenario20(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_reliable_subscriber_receives_battery_with_override(self):
        global _SPAWNED
        node = rclpy.create_node('e2e_s20_probe')
        try:
            bridge_support.wait_for_bridge(node)
            received_diag = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received_diag.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received_diag:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            received_diag.clear()
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ('--rate', '10'))
            link_ready_at = _wait_link_ready(node, received_diag, _SPAWNED.ready_at)
            checks.wait_for_handshake(
                received_diag,
                HANDSHAKE_TOPICS,
                link_ready_at,
                TOPIC_BUDGET,
                'handshake',
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
                clock=time.monotonic,
            )
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node,
                ['/battery'],
                handshake_done_at,
                checks.DISCOVERY_TIMEOUT,
                'battery topic',
                lambda: rclpy.spin_once(node, timeout_sec=bridge_support.DISCOVERY_SPIN),
            )
            received = []
            # Default rclpy QoS asks for reliable delivery.
            node.create_subscription(Float32, 'battery', lambda msg: received.append(msg.data), 10)
            deadline = time.monotonic() + FLOW_TIMEOUT
            while len(received) < 3:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no Float32 flow on battery')
        finally:
            node.destroy_node()
