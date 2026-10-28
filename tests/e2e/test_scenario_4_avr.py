"""Scenario 4: namespace and remappings apply to the MCU's topics.

The bridge is included under a namespace push and two remap rules; the
announced topics show up renamed and namespaced, and the original names stay
silent.
"""

import os
import tempfile
import time
import unittest

import launch_testing.markers
import rclpy
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support

# covers: SES-16, SES-27

NAMESPACE = "e2e_ns"
SERVICE = "/" + NAMESPACE + "/r2s_bridge/get_parameters"
REMAPPINGS = (("battery", "cell_voltage"), ("cmd_vel", "steer"))
WANTED = ["/" + NAMESPACE + "/cell_voltage", "/" + NAMESPACE + "/steer"]
GONE = ["/battery", "/cmd_vel", "/" + NAMESPACE + "/battery", "/" + NAMESPACE + "/cmd_vel"]

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-avr-4-")
    _SPAWNED = avr_support.spawn(os.environ.get("R2S_E2E_BOARD", "uno"))
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_SPAWNED.pty, _PARAMS, NAMESPACE, REMAPPINGS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
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
        node = rclpy.create_node("e2e_s4_avr_probe")
        try:
            bridge_support.wait_for_bridge(node, service=SERVICE)
            _SPAWNED.ready_at = time.monotonic()
            bridge_support.wait_for_topics(node, WANTED, _SPAWNED.ready_at, 3.0, "remapped topics")
            names = {name for name, _ in node.get_topic_names_and_types()}
            for gone in GONE:
                self.assertNotIn(gone, names, "unmapped name still announced: " + gone)
        finally:
            node.destroy_node()
