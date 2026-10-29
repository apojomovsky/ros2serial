"""Scenario 4: namespace and remappings apply to the MCU's topics.

The announced topics show up renamed and namespaced, and the original
names stay silent. The bridge is up before the firmware boots, so the 3 s
budget measures the handshake from the MCU's HELLO, not node startup.
Graph discovery of the renamed topics waits on its own generous timeout,
so a slow runner cannot fail a correct remapping.
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
from e2e_support import checks, firmware
from e2e_support.relay import PtyRelay

# covers: SES-16, SES-27

NAMESPACE = "e2e_ns"
SERVICE = "/" + NAMESPACE + "/r2s_bridge/get_parameters"
REMAPPINGS = (("battery", "cell_voltage"), ("cmd_vel", "steer"))
WANTED = ["/" + NAMESPACE + "/cell_voltage", "/" + NAMESPACE + "/steer"]
GONE = ["/battery", "/cmd_vel", "/" + NAMESPACE + "/battery", "/" + NAMESPACE + "/cmd_vel"]
# The bridge reports announced names without remapping or namespace.
HANDSHAKE_TOPICS = ["battery", "cmd_vel"]
TOPIC_BUDGET = 3.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-4-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = firmware.from_run_script() if "R2S_E2E_FW" in os.environ else firmware.build(scratch)
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


class TestScenario4(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_remapping_and_namespace_apply_to_mcu_topics(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s4_probe")
        try:
            bridge_support.wait_for_bridge(node, service=SERVICE)
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
            while True:
                rclpy.spin_once(node, timeout_sec=0.5)
                missing = checks.handshake_missing(received[-1], HANDSHAKE_TOPICS) if received else ["link silent"]
                took = time.monotonic() - _SPAWNED.ready_at
                if not missing:
                    break
                self.assertLess(took, TOPIC_BUDGET, f"handshake still missing {missing} after {took:.1f} s")
            self.assertLessEqual(took, TOPIC_BUDGET, "handshake took longer than 3 s")
            bridge_support.wait_for_topics(node, WANTED, _SPAWNED.ready_at, checks.DISCOVERY_TIMEOUT, "remapped topics")
            names = {name for name, _ in node.get_topic_names_and_types()}
            for gone in GONE:
                self.assertNotIn(gone, names, "unmapped name still announced: " + gone)
        finally:
            node.destroy_node()
