"""Scenario 14: a service call from ROS 2 reaches the MCU and back, both ways.

The firmware serves reset and calls trigger (both std_srvs Trigger). The
ROS client calls /reset and reads the MCU's answer, proving the
ROS-initiated round trip with MCU-generated content. The ROS server on
/trigger answers the MCU's call, and the firmware reports the outcome on
log_text, proving the MCU-initiated round trip the same way. The server is
created before the firmware boots, and the service proxies exist from the
handshake, so the MCU's single call per connection always finds it.
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
from rclpy.qos import QoSProfile
from rclpy.qos import ReliabilityPolicy
from std_msgs.msg import String
from std_srvs.srv import Trigger

# covers: SVR-4, SVR-5, SVR-6

ANNOUNCED = ['/battery', '/cmd_vel', '/echo_vel', '/log_text', '/time_now']
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix('/') for name in ANNOUNCED]
TOPIC_BUDGET = 3.0
SERVICE_TIMEOUT = 20.0
CALL_TIMEOUT = 10.0
# The MCU reports its call's outcome as 8 hex digits on log_text, with the
# status byte low: 5e000000 is the ok answer.
MARKER_OK = '5e000000'

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-14-')
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
        rclpy.spin_once(node, timeout_sec=0.2)


def _c_firmware():
    """Whether this leg boots the C firmware, the only shape serving services yet."""
    given = os.environ.get('R2S_E2E_FW', '')
    return not given or given.endswith('/r2s_posix_firmware')


@unittest.skipUnless(
    _c_firmware(),
    "scenario 14 needs the C firmware's services; the C++ shapes have no service API yet",
)
class TestScenario14(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_service_calls_round_trip_both_directions(self):
        global _SPAWNED
        node = rclpy.create_node('e2e_s14_probe')
        try:
            bridge_support.wait_for_bridge(node)
            # The server exists before the firmware boots, and the bridge
            # builds its client proxy at the handshake, so the MCU's single
            # call per connection always finds it.
            served = []
            server = node.create_service(
                Trigger,
                '/trigger',
                lambda request, response: served.append(request) or self._answer_trigger(response),
            )
            try:
                # Subscribed before the firmware boots: the MCU reports its
                # call's outcome right after the handshake, and a best-effort
                # sample has no queue for a subscription the bridge has not
                # discovered yet.
                best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)
                markers = []
                node.create_subscription(
                    String, '/log_text', lambda msg: markers.append(msg.data), best_effort
                )
                received = []
                node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
                deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
                while not received:
                    rclpy.spin_once(node, timeout_sec=0.5)
                    self.assertLess(
                        time.monotonic(), deadline, 'no /diagnostics flow from the bridge'
                    )
                received.clear()
                _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ('--rate', '10'))
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
                handshake_done_at = time.monotonic()
                bridge_support.wait_for_topics(
                    node,
                    ANNOUNCED,
                    handshake_done_at,
                    checks.DISCOVERY_TIMEOUT,
                    'announced topics',
                    lambda: rclpy.spin_once(node, timeout_sec=bridge_support.DISCOVERY_SPIN),
                )
                # The ROS call reaches the MCU and comes back with content
                # only the firmware produces.
                client = node.create_client(Trigger, '/reset')
                try:
                    self.assertTrue(
                        client.wait_for_service(timeout_sec=SERVICE_TIMEOUT),
                        'the reset service never appeared',
                    )
                    future = client.call_async(Trigger.Request())
                    deadline = time.monotonic() + CALL_TIMEOUT
                    while not future.done():
                        rclpy.spin_once(node, timeout_sec=0.2)
                        self.assertLess(
                            time.monotonic(), deadline, 'the reset call never answered'
                        )
                    response = future.result()
                    self.assertTrue(response.success, 'the MCU refused reset')
                    self.assertEqual(response.message, 'ok', 'the MCU answer carries its own text')
                finally:
                    node.destroy_client(client)
                # The MCU's call reaches the ROS server, whose answer reaches
                # the MCU: the firmware reported the outcome on log_text.
                deadline = time.monotonic() + CALL_TIMEOUT
                while MARKER_OK not in markers:
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(
                        time.monotonic(), deadline, 'the MCU never reported its call outcome'
                    )
                self.assertTrue(served, 'the ROS server never saw the trigger call')
            finally:
                node.destroy_service(server)
        finally:
            node.destroy_node()

    @staticmethod
    def _answer_trigger(response):
        response.success = True
        response.message = 'server-ok'
        return response
