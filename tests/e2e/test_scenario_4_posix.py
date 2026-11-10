# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 4: namespace and remappings apply to the MCU's topics.

The announced topics show up renamed and namespaced, and the original
names stay silent. The bridge is up before the firmware boots. The
handshake first waits out the boot wait on the generous timeout, then runs
the unchanged 3 s budget from the later of the MCU's HELLO and the boot
wait end, so a slow bridge open cannot spend the handshake budget. Graph
discovery of the renamed topics waits on its own generous timeout from the
handshake, so a slow runner cannot fail a correct remapping.
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

# covers: SES-16, SES-27

NAMESPACE = 'e2e_ns'
SERVICE = '/' + NAMESPACE + '/r2s_bridge/get_parameters'
REMAPPINGS = (('battery', 'cell_voltage'), ('cmd_vel', 'steer'))
WANTED = ['/' + NAMESPACE + '/cell_voltage', '/' + NAMESPACE + '/steer']
GONE = ['/battery', '/cmd_vel', '/' + NAMESPACE + '/battery', '/' + NAMESPACE + '/cmd_vel']
# The bridge reports announced names without remapping or namespace.
HANDSHAKE_TOPICS = ['battery', 'cmd_vel', 'echo_vel', 'log_text', 'time_now']
TOPIC_BUDGET = 3.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-4-')
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = (
        firmware.from_run_script() if 'R2S_E2E_FW' in os.environ else firmware.build(scratch)
    )
    _RELAY = PtyRelay()
    _RELAY.start()
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_RELAY.host_port, _PARAMS, NAMESPACE, REMAPPINGS)


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
        if received and received[-1].status[0].message not in ('disconnected', 'boot_wait'):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, (
            f'link never left boot wait after {took:.1f} s'
        )
        rclpy.spin_once(node, timeout_sec=0.5)


class TestScenario4(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_remapping_and_namespace_apply_to_mcu_topics(self):
        global _SPAWNED
        node = rclpy.create_node('e2e_s4_probe')
        try:
            bridge_support.wait_for_bridge(node, service=SERVICE)
            # Matched before the firmware boots, so the handshake wait below
            # observes bridge state, not DDS discovery of the new topics.
            received = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            received.clear()
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ('--rate', '10'))
            # The boot wait is the bridge's own delay before the handshake
            # starts, so it waits on the generous timeout and the 3 s
            # budget runs from the later of the ready line and its end.
            link_ready_at = _wait_link_ready(node, received, _SPAWNED.ready_at)
            took = checks.wait_for_handshake(
                received,
                HANDSHAKE_TOPICS,
                link_ready_at,
                TOPIC_BUDGET,
                'handshake',
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
                clock=time.monotonic,
            )
            self.assertLessEqual(took, TOPIC_BUDGET, 'handshake took longer than 3 s')
            # Discovery starts when the bridge creates its endpoints at the
            # handshake, so the generous timeout runs from there, not from
            # the firmware spawn: a slow handshake must not spend it.
            handshake_done_at = time.monotonic()
            bridge_support.wait_for_topics(
                node,
                WANTED,
                handshake_done_at,
                checks.DISCOVERY_TIMEOUT,
                'remapped topics',
                lambda: rclpy.spin_once(node, timeout_sec=bridge_support.DISCOVERY_SPIN),
            )
            names = {name for name, _ in node.get_topic_names_and_types()}
            for gone in GONE:
                self.assertNotIn(gone, names, 'unmapped name still announced: ' + gone)
        finally:
            node.destroy_node()
