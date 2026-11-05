"""Scenario 12: the MCU publishes more than the link carries.

The firmware floods battery samples through a per-tick byte budget, so a
publish that finds the budget dry returns TX_BUSY and its sample never
takes a seq. The host stream stays integral and ordered, with gaps where
samples were shed, and the bridge seq gaps stay 0 while the firmware busy
count climbs: the drops are counted where they happen, and no frame on
the wire is corrupted by them.
"""

import os
import select
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import bridge as bridge_support
from e2e_support import checks, firmware, teardown
from e2e_support.relay import PtyRelay
from rcl_interfaces.msg import Log
from rclpy.qos import QoSProfile, ReliabilityPolicy
from std_msgs.msg import Float32

# covers: ERR-3, ERR-6

WINDOW_S = 10.0
TX_FREE = "16"
# The flood moves thousands of samples; this floor only proves the flow
# is alive, since shed samples are the point of the scenario.
RECEIVE_FLOOR = 100

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-12-")
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


def _flood_busy(proc):
    """(sent, busy) from the last flood report the firmware left on stderr."""
    fd = proc.stderr.fileno()
    out = bytearray()
    while True:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if not ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    sent = 0
    busy = 0
    for line in out.decode(errors="replace").splitlines():
        if not line.startswith("r2s_posix: flood sent="):
            continue
        for part in line.split():
            if part.startswith("sent="):
                sent = int(part[len("sent=") :])
            elif part.startswith("busy="):
                busy = int(part[len("busy=") :])
    return sent, busy


class TestScenario12(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_saturation_sheds_samples_with_busy_and_whole_frames(self):
        global _SPAWNED
        node = rclpy.create_node("e2e_s12_probe")
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, "/diagnostics", received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no /diagnostics flow from the bridge")
            _SPAWNED = firmware.spawn(
                _FIRMWARE,
                _RELAY.mcu_port,
                (
                    "--no-sub",
                    "--no-echo",
                    "--no-log-text",
                    "--no-time-now",
                    "--tx-free",
                    TX_FREE,
                    "--flood",
                ),
            )
            # The bridge publishes MCU topics best effort, so the probe
            # subscribes best effort too; a reliable probe would never match.
            best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
            values = []
            node.create_subscription(Float32, "battery", lambda msg: values.append(msg.data), best_effort)
            rosout = []
            node.create_subscription(Log, "/rosout", rosout.append, best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not values:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, "no Float32 flow on battery")
            start = time.monotonic()
            window_end = start + WINDOW_S
            while time.monotonic() < window_end:
                rclpy.spin_once(node, timeout_sec=0.5)
            missing = checks.handshake_missing(received[-1], ["battery"])
            self.assertFalse(missing, f"link not connected: {missing}")
            self.assertGreaterEqual(len(values), RECEIVE_FLOOR, "the flood left no flow")
            for value in values:
                self.assertTrue(float(value).is_integer(), f"corrupt value {value!r}")
            for earlier, later in zip(values, values[1:], strict=False):
                self.assertGreater(later, earlier, f"values out of order: {later!r} after {earlier!r}")
            span = int(values[-1]) - int(values[0]) + 1
            self.assertGreater(span - len(values), 0, "no sample was shed on a saturated link")
            sent, busy = _flood_busy(_SPAWNED.proc)
            self.assertGreater(sent, 0, "the firmware reported no flood samples")
            self.assertGreater(busy, 0, "no publish returned TX_BUSY on a saturated link")
            gaps = {value.key: value.value for value in received[-1].status[0].values}
            self.assertEqual(gaps.get("topic battery seq gaps"), "0", "shed samples took a seq")
            # Battery is the first announced topic, so its diagnostics id is 16.
            self.assertGreater(int(gaps.get("mcu error code 6", "0")), 0, "busy sum missing from /diagnostics")
            breakdown = gaps.get("topic 16 battery mcu errors", "")
            slots = dict(part.split("=") for part in breakdown.split())
            self.assertGreater(int(slots.get("6", "0")), 0, "battery breakdown misses the busy count")
            reports = [
                entry.msg
                for entry in rosout
                if entry.name == "r2s_bridge.mcu" and "error report" in entry.msg and "code 6" in entry.msg
            ]
            self.assertGreater(len(reports), 0, "no busy report reached the bridge")
            self.assertLessEqual(
                len(reports),
                WINDOW_S + 5,
                f"busy reports {len(reports)} exceed the once-per-second limit over {WINDOW_S} s",
            )
        finally:
            node.destroy_node()
