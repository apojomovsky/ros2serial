"""Scenario 11: a subscribed message that exceeds a bound is dropped, and the
bridge logs the topic and the field path.

The host publishes a valid JointState, which the firmware counts; then one
with seven names against the bound of six. The firmware never counts the
second one: the decoder refuses it, the firmware reports code 1 with the
field, and the bridge logs the topic and the field path on /rosout. A third
valid message still arrives, so a dead link cannot fake the drop.
"""

import os
import re
import select
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import bridge as bridge_support
from e2e_support import firmware, teardown
from e2e_support.relay import PtyRelay
from rcl_interfaces.msg import Log
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import JointState

# covers: ERR-1, ERR-4, ERR-5

LOGGER_NAME = "r2s_bridge.mcu"
ROSOUT_BUDGET = 30.0
PHASE_BUDGET = 30.0
QUIESCE_S = 2.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _valid(names, positions):
    msg = JointState()
    msg.header.frame_id = ""
    msg.name = list(names)
    msg.position = list(positions)
    return msg


VALID = _valid(["a", "b"], [1.0, -1.0])
OVERSIZED = _valid(["j0", "j1", "j2", "j3", "j4", "j5", "j6"], [0.0] * 7)


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-11-")
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


def _firmware_lines(proc):
    """Lines the firmware has printed since the last call, without blocking."""
    readable, _, _ = select.select([proc.stderr], [], [], 0.0)
    chunk = b""
    if proc.stderr in readable:
        chunk = os.read(proc.stderr.fileno(), 65536)
    return chunk.decode("utf-8", "replace")


def _joint_counts(text):
    """Every joints arrival count the firmware has printed in text."""
    return [int(m.group(1)) for m in re.finditer(r"r2s_posix: joints n=(\d+)", text)]


class TestScenario11(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def _wait_for(self, node, deadline, what, seen):
        while not seen():
            rclpy.spin_once(node, timeout_sec=0.5)
            self.assertLess(time.monotonic(), deadline, what)

    def test_oversized_subscription_is_dropped_and_logged(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s11_probe")
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            received.clear()
            logs = []
            qos = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            node.create_subscription(Log, "/rosout", logs.append, qos)
            # The bridge subscribes to MCU topics best effort, so the probe
            # publishes best effort too; a reliable probe would never match.
            joints = node.create_publisher(JointState, "joints", qos)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            log = []
            deadline = time.monotonic() + PHASE_BUDGET
            while not _joint_counts("".join(log)):
                joints.publish(VALID)
                rclpy.spin_once(node, timeout_sec=0.5)
                log.append(_firmware_lines(_SPAWNED.proc))
                self.assertLess(time.monotonic(), deadline, "valid JointState never arrived")
            # Quiesce: valid messages already in flight still land after the
            # first count, so drain them before fixing the baseline; under
            # load several arrive here, and none may leak into the drop phase.
            stop = time.monotonic() + QUIESCE_S
            while time.monotonic() < stop:
                rclpy.spin_once(node, timeout_sec=0.5)
                log.append(_firmware_lines(_SPAWNED.proc))
            baseline = max(_joint_counts("".join(log)))
            logs.clear()
            deadline = time.monotonic() + PHASE_BUDGET
            report = None
            while report is None:
                joints.publish(OVERSIZED)
                rclpy.spin_once(node, timeout_sec=0.5)
                log.append(_firmware_lines(_SPAWNED.proc))
                for entry in logs:
                    if entry.name == LOGGER_NAME and "code 1 on topic 'joints'" in entry.msg:
                        report = entry
                self.assertLess(time.monotonic(), deadline, "no code 1 report on /rosout")
            self.assertIn("at field 'name'", report.msg)
            self.assertEqual(report.level, 30)
            self.assertEqual(max(_joint_counts("".join(log))), baseline, "oversized JointState reached the firmware")
            deadline = time.monotonic() + PHASE_BUDGET
            while max(_joint_counts("".join(log))) <= baseline:
                joints.publish(VALID)
                rclpy.spin_once(node, timeout_sec=0.5)
                log.append(_firmware_lines(_SPAWNED.proc))
                self.assertLess(time.monotonic(), deadline, "link died after the drop")
        finally:
            node.destroy_node()
