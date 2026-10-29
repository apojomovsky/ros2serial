"""Scenario 9: the MCU's log lines appear on /rosout at every level.

The firmware emits one plain line per level while connected; the bridge
forwards each through its mcu child logger, so /rosout carries them with
the right severity and the bridge's logger name.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from e2e_support import bridge as bridge_support
from e2e_support import firmware
from e2e_support.relay import PtyRelay
from rcl_interfaces.msg import Log
from rclpy.qos import QoSProfile, ReliabilityPolicy

# covers: LOG-1, LOG-2

EXPECTED = {
    10: "r2s log debug",
    20: "r2s log info",
    30: "r2s log warn",
    40: "r2s log error",
    50: "r2s log fatal",
}
LOGGER_NAME = "r2s_bridge.mcu"
LOG_BUDGET = 30.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-9-")
    scratch = Path(_SCRATCH.name)
    _FIRMWARE = firmware.from_run_script() if "R2S_E2E_FW" in os.environ else firmware.build(scratch)
    _RELAY = PtyRelay()
    _RELAY.start()
    _PARAMS = bridge_support.example_params_file()


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_RELAY.host_port, _PARAMS, log_level="debug")


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _RELAY is not None:
        _RELAY.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


class TestScenario9(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_mcu_log_lines_reach_rosout_with_severity_and_name(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s9_probe")
        try:
            bridge_support.wait_for_bridge(node)
            # Best effort matches whatever reliability rosout publishes
            # with, and the firmware emits every 2 s, so a line missed
            # before the subscription is up arrives again on its own.
            received = []
            qos = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            node.create_subscription(Log, "/rosout", received.append, qos)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10", "--log-lines"))
            deadline = time.monotonic() + LOG_BUDGET
            seen = {}
            while len(seen) < len(EXPECTED):
                rclpy.spin_once(node, timeout_sec=0.5)
                for entry in received:
                    if entry.name == LOGGER_NAME and entry.msg in EXPECTED.values():
                        seen[entry.msg] = entry.level
                received.clear()
                self.assertLess(time.monotonic(), deadline, f"only saw {sorted(seen)} on /rosout")
            for level, text in EXPECTED.items():
                self.assertEqual(seen[text], level, f"{text} arrived with the wrong severity")
        finally:
            node.destroy_node()
