"""Baud matrix: the scenario 2 flow gates at one rate of the matrix.

run-avr.sh builds one slice image per board per baud in R2S_E2E_BAUDS and
runs this file under the first RMW only, so the matrix stays cheap. The
firmware baud travels as R2S_BAUD at compile time and the bridge baud as a
temp copy of the example params; the launch file and the shared helpers stay
untouched. The gates mirror test_scenario_2_avr.py with the same margins.
"""

import os
import re
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks, teardown
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: SES-1, SES-2, SES-5, SES-6, SES-13, SES-16, SES-17

# The same margins as scenario 2: 500 publishes, at most 5 lost, and the
# simulated span of a 50 Hz flow. Baud changes the byte timing, not the tick.
RECEIVE_COUNT = 500
LOSS_BUDGET = 5
FLOW_TIMEOUT = 120.0

_SCRATCH = None
_SPAWNED = None
_PARAMS = None


def _params_with_baud(scratch, baud):
    """A temp copy of the example params serving the matrix baud."""
    assert baud.isdigit(), f"R2S_E2E_BAUD names no baud rate: {baud!r}"
    text = Path(bridge_support.example_params_file()).read_text(encoding="utf-8")
    updated, count = re.subn(r"(?m)^(\s*baud:\s*)\d+\s*$", rf"\g<1>{baud}", text)
    assert count == 1, "the example params hold no single baud line"
    path = Path(scratch) / "r2s_bridge.params.yaml"
    path.write_text(updated, encoding="utf-8")
    return path


def _prepare():
    global _SCRATCH, _SPAWNED, _PARAMS
    board = os.environ.get("R2S_E2E_BOARD", "uno")
    baud = os.environ["R2S_E2E_BAUD"]
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-avr-baud-")
    _SPAWNED = avr_support.spawn(board, "slice", baud=baud)
    _PARAMS = _params_with_baud(_SCRATCH.name, baud)


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    return bridge_support.launch_bridge_only(_SPAWNED.pty, _PARAMS)


def tearDownModule():
    if _SPAWNED is not None:
        _SPAWNED.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


class TestBaudMatrix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_50hz_flow_arrives_nearly_whole_at_the_matrix_baud(self):
        node = rclpy.create_node("e2e_baud_avr_probe")
        try:
            bridge_support.wait_for_bridge(node)
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            received = []
            node.create_subscription(Float32, "battery", lambda msg: received.append(msg.data), best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            checks.collect_until(
                received,
                RECEIVE_COUNT,
                time.monotonic() + FLOW_TIMEOUT,
                "battery at 50 Hz",
                lambda: rclpy.spin_once(node, timeout_sec=0.5),
            )
            checks.check_float_sequence(list(received), RECEIVE_COUNT, LOSS_BUDGET, "battery at 50 Hz")
            self.assertEqual(_SPAWNED.stop(), 0, "the harness stopped dirty")
            self.assertIn("simulated_ms", _SPAWNED.summary, "no stop summary from the harness")
            sim_ms = _SPAWNED.summary["simulated_ms"]
            self.assertGreaterEqual(sim_ms, 9_000, f"500 publishes in {sim_ms} ms simulated, faster than 50 Hz")
            self.assertLessEqual(sim_ms, 45_000, f"500 publishes in {sim_ms} ms simulated, slower than 50 Hz")
        finally:
            node.destroy_node()
