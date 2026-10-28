"""Scenario 3: the host publishes Twist and the MCU echoes it back.

Five distinct Twists go out on cmd_vel; the firmware republishes each on
echo_vel, and every echoed field matches what was sent.
"""

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from e2e_support import bridge as bridge_support
from e2e_support import checks, firmware
from e2e_support.relay import PtyRelay
from geometry_msgs.msg import Twist
from rclpy.qos import QoSProfile, ReliabilityPolicy

# covers: SES-9, SES-18, SES-31

ECHO_COUNT = 5
FLOW_TIMEOUT = 20.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _twist(linear, angular):
    msg = Twist()
    msg.linear.x, msg.linear.y, msg.linear.z = linear
    msg.angular.x, msg.angular.y, msg.angular.z = angular
    return msg


SENT = [
    _twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001)),
    _twist((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
    _twist((-7.75, 42.0, -0.125), (3.0, -3.0, 9.5)),
    _twist((1000.0, -1000.0, 0.5), (-0.25, 0.25, -44.0)),
    _twist((2.5, 2.5, 2.5), (-2.5, -2.5, -2.5)),
]

# The priming Twist, outside SENT: the priming loop repeats it until one echo
# gets through, and echoes of repeats still in flight afterwards are filtered
# out of the sequence under test instead of shifting it.
PRIME = _twist((9.75, -8.5, 7.25), (-6.5, 5.5, -4.75))


def _fields(msg):
    return (
        msg.linear.x,
        msg.linear.y,
        msg.linear.z,
        msg.angular.x,
        msg.angular.y,
        msg.angular.z,
    )


def _is_prime(msg):
    return _fields(msg) == _fields(PRIME)


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix="r2s-e2e-3-")
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


class TestScenario3(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def test_published_twists_come_back_on_the_echo_topic(self):
        global _SPAWNED
        self.assertTrue(all(not _is_prime(msg) for msg in SENT), "prime collides with the sequence")
        node = rclpy.create_node("e2e_s3_probe")
        try:
            bridge_support.wait_for_bridge(node)
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ("--rate", "10"))
            # The bridge forwards MCU traffic best effort in both directions,
            # so both ends of the probe match that; reliable ends never match.
            best_effort = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
            arrived = []
            node.create_subscription(Twist, "echo_vel", arrived.append, best_effort)
            pub = node.create_publisher(Twist, "cmd_vel", best_effort)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not arrived:
                pub.publish(PRIME)
                rclpy.spin_once(node, timeout_sec=0.2)
                self.assertLess(time.monotonic(), deadline, "no echo flow on echo_vel")
            for msg in SENT:
                pub.publish(msg)
                time.sleep(0.2)
            deadline = time.monotonic() + FLOW_TIMEOUT
            sequence = []
            while len(sequence) < ECHO_COUNT:
                self.assertLess(time.monotonic(), deadline, f"echo_vel: only {len(sequence)} of {ECHO_COUNT} arrived")
                rclpy.spin_once(node, timeout_sec=0.2)
                sequence = [msg for msg in arrived if not _is_prime(msg)]
            checks.check_twist_echo(SENT, sequence[:ECHO_COUNT], "echo_vel")
        finally:
            node.destroy_node()
