"""Scenario 13: a ROS parameter set reaches the MCU.

The bridge declares the firmware's parameters as its own ROS parameters
after the handshake. The test sets the bool and the i64 through the ROS
parameter API and waits for the firmware's applied lines on its stderr,
proving the values crossed the reliable channel to the MCU.
"""

# covers: PAR-9

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
from e2e_support import checks
from e2e_support import firmware
from e2e_support import teardown
from e2e_support.relay import PtyRelay
from rclpy.parameter import Parameter
from rclpy.parameter_client import AsyncParameterClient

ANNOUNCED = ['/battery', '/cmd_vel', '/echo_vel', '/log_text', '/time_now']
HANDSHAKE_TOPICS = [name.removeprefix('/') for name in ANNOUNCED]
TOPIC_BUDGET = 3.0
PARAM_TIMEOUT = 20.0
APPLIED_BUDGET = 10.0

_SCRATCH = None
_FIRMWARE = None
_RELAY = None
_SPAWNED = None
_PARAMS = None


def _prepare():
    global _SCRATCH, _FIRMWARE, _RELAY, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-13-')
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
    """Spin until the link leaves boot wait, within the generous timeout.

    The bridge opens the port and waits out boot_wait_ms for an
    unsolicited HELLO before the handshake starts, and under load the
    open itself can lag the firmware ready line. The 3 s budget runs
    from the later of the ready line and this return.
    """
    while True:
        if received and received[-1].status[0].message not in ('disconnected', 'boot_wait'):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, (
            f'link never left boot wait after {took:.1f} s'
        )
        rclpy.spin_once(node, timeout_sec=0.5)


def _wait_ros_params(client, node, names, budget):
    """Poll the bridge until it declares every MCU parameter, on the budget.

    Listing names is the declaration check: getting an undeclared name
    answers with an unset value instead of failing.
    """
    since = time.monotonic()
    while True:
        try:
            future = client.list_parameters()
            rclpy.spin_until_future_complete(node, future, timeout_sec=2.0)
            result = future.result()
            if result is not None and all(name in result.result.names for name in names):
                return
        except Exception:
            pass
        took = time.monotonic() - since
        assert took < budget, f'bridge never declared MCU parameters: {sorted(names)}'
        rclpy.spin_once(node, timeout_sec=0.1)


def _wait_applied_lines(proc, needles, budget):
    """Read the firmware's stderr until every needle landed, on the budget."""
    pending = list(needles)
    seen = b''
    deadline = time.monotonic() + budget
    while pending and time.monotonic() < deadline:
        readable, _, _ = select.select([proc.stderr], [], [], 0.5)
        if proc.stderr in readable:
            chunk = os.read(proc.stderr.fileno(), 4096)
            if chunk:
                seen += chunk
        for needle in list(pending):
            if needle.encode() in seen:
                pending.remove(needle)
    assert not pending, f'firmware never applied: {pending}'


def _params_firmware():
    """Whether this leg boots a firmware carrying the parameter table."""
    given = os.environ.get('R2S_E2E_FW', '')
    return not given or not given.endswith('/r2s_posix_static_firmware')


@unittest.skipUnless(
    _params_firmware(),
    "scenario 13 needs the firmware's parameter table; the static C++ shape carries none",
)
class TestScenario13(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def test_ros_parameter_set_reaches_the_mcu(self):
        global _SPAWNED
        node = rclpy.create_node('e2e_s13_probe')
        try:
            bridge_support.wait_for_bridge(node)
            received = []
            node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
            deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
            while not received:
                rclpy.spin_once(node, timeout_sec=0.5)
                self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
            _SPAWNED = firmware.spawn(_FIRMWARE, _RELAY.mcu_port, ('--rate', '10'))
            received.clear()
            link_ready_at = _wait_link_ready(node, received, _SPAWNED.ready_at)
            checks.wait_for_handshake(
                received,
                HANDSHAKE_TOPICS,
                link_ready_at,
                TOPIC_BUDGET,
                'handshake',
                lambda: rclpy.spin_once(node, timeout_sec=checks.HANDSHAKE_SPIN),
                clock=time.monotonic,
            )
            client = AsyncParameterClient(node, '/r2s_bridge')
            self.assertTrue(
                client.wait_for_services(timeout_sec=PARAM_TIMEOUT), 'no parameter service'
            )
            _wait_ros_params(client, node, ['led', 'rate'], PARAM_TIMEOUT)
            future = client.set_parameters(
                [
                    Parameter('led', Parameter.Type.BOOL, False),
                    Parameter('rate', Parameter.Type.INTEGER, 5),
                ]
            )
            rclpy.spin_until_future_complete(node, future, timeout_sec=PARAM_TIMEOUT)
            self.assertIsNotNone(future.result(), 'parameter set never answered')
            for result in future.result().results:
                self.assertTrue(result.successful, f'parameter set refused: {result.reason}')
            _wait_applied_lines(_SPAWNED.proc, ['param led = 0', 'param rate = 5'], APPLIED_BUDGET)
        finally:
            node.destroy_node()
