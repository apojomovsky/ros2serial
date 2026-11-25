# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 8 judges its stamps through the least-delayed helper.

Regression test for #660: the helper's unit tests cannot see the call
site, so reverting the scenario to an all-stamp median stayed green.
This drives the posix scenario module with recording fakes and a stepped
clock, pins the judge call it makes, and needs no ROS.
"""

import importlib.util
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / 'tests' / 'e2e'
POSIX_8 = E2E / 'test_scenario_8_posix.py'

STEP = 0.25

_current = None


def _events():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.events


def _nodes():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.nodes


def _clock():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.clock


class FakeClock:
    """A stepped clock: every spin advances it, so timestamps stay exact."""

    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FakeValue:
    def __init__(self, key, value):
        self.key = key
        self.value = value


class FakeStatus:
    def __init__(self, message, values):
        self.message = message
        self.values = [FakeValue(key, value) for key, value in values]


class FakeDiagnostics:
    def __init__(self, message, values=()):
        self.status = [FakeStatus(message, values)]


class FakeQoS:
    def __init__(self, depth=None, reliability=None):
        self.depth = depth
        self.reliability = reliability


class FakeNode:
    """A probe node whose every spin reports a settled, connected link."""

    def __init__(self, events, clock, topics):
        self._events = events
        self._subs = {}
        self._clock = clock
        self._topics = topics

    def create_subscription(self, msg_type, topic, callback, qos):
        self._events.append(('sub', topic))
        self._subs.setdefault(topic, []).append(callback)
        return ('sub', topic)

    def get_clock(self):
        clock = self._clock
        return types.SimpleNamespace(
            now=lambda: types.SimpleNamespace(nanoseconds=int(clock.now * 1_000_000_000))
        )

    def destroy_node(self):
        self._events.append(('destroy_node',))

    def spin_once(self):
        self._clock.advance(STEP)
        values = [(f'topic {16 + i} {name} status', 'ok') for i, name in enumerate(self._topics)]
        values += [
            ('time sync offset', '0'),
            ('time sync drift', '-4975'),
            ('time sync round trip', '1000000'),
        ]
        self.deliver('/diagnostics', FakeDiagnostics('connected', values))
        host_ns = int(self._clock.now * 1_000_000_000)
        stamp = types.SimpleNamespace(
            sec=host_ns // 1_000_000_000, nanosec=host_ns % 1_000_000_000
        )
        for _ in range(3):
            self.deliver('time_now', stamp)

    def deliver(self, topic, msg):
        for callback in self._subs.get(topic, []):
            callback(msg)


_STUB_NAMES = (
    'launch_testing',
    'launch_testing.markers',
    'rclpy',
    'rclpy.qos',
    'builtin_interfaces',
    'builtin_interfaces.msg',
    'diagnostic_msgs',
    'diagnostic_msgs.msg',
    'e2e_support',
    'e2e_support.bridge',
    'e2e_support.checks',
    'e2e_support.firmware',
    'e2e_support.relay',
    'e2e_support.teardown',
)


def _install_stubs():
    """Recording fakes for the ROS imports the scenario module needs.

    Everything is installed here, at test time, so this file never depends
    on which other stub-based test module pytest imported first. Nothing
    here touches the real e2e_support package: the checks helper loads
    straight from its file, keeping the stub package pathless, so a missing
    fake fails loudly instead of importing the real backend.
    """
    spec = importlib.util.spec_from_file_location(
        's8_judge_real_checks', E2E / 'e2e_support' / 'checks.py'
    )
    real_checks = importlib.util.module_from_spec(spec)
    sys.modules['s8_judge_real_checks'] = real_checks
    spec.loader.exec_module(real_checks)

    judge = real_checks.least_delayed_error_ns

    def least_delayed_error_ns(stamps, rtts, keep):
        _events().append(('judge', list(stamps), list(rtts), keep))
        return judge(stamps, rtts, keep)

    real_checks.least_delayed_error_ns = least_delayed_error_ns

    launch_testing = types.ModuleType('launch_testing')
    markers = types.ModuleType('launch_testing.markers')
    markers.keep_alive = lambda func: func
    launch_testing.markers = markers

    rclpy = types.ModuleType('rclpy')
    qos = types.ModuleType('rclpy.qos')
    qos.QoSProfile = FakeQoS
    qos.ReliabilityPolicy = type('ReliabilityPolicy', (), {'BEST_EFFORT': 1})
    rclpy.qos = qos

    def create_node(name):
        node = FakeNode(_events(), _clock(), _current.topics)
        _nodes().append(node)
        return node

    rclpy.create_node = create_node
    rclpy.init = lambda: _events().append(('init',))
    rclpy.spin_once = lambda node, timeout_sec=0.0: node.spin_once()

    builtin_interfaces = types.ModuleType('builtin_interfaces')
    builtin_interfaces_msg = types.ModuleType('builtin_interfaces.msg')
    builtin_interfaces_msg.Time = type('Time', (), {})
    builtin_interfaces.msg = builtin_interfaces_msg

    diagnostic_msgs = types.ModuleType('diagnostic_msgs')
    diagnostic_msgs_msg = types.ModuleType('diagnostic_msgs.msg')
    diagnostic_msgs_msg.DiagnosticArray = type('DiagnosticArray', (), {})
    diagnostic_msgs.msg = diagnostic_msgs_msg

    bridge = types.ModuleType('e2e_support.bridge')
    bridge.HANDSHAKE_TIMEOUT = 20.0
    bridge.wait_for_bridge = lambda node: _events().append(('wait_for_bridge',))
    bridge.launch_bridge_only = lambda *args, **kwargs: None
    bridge.example_params_file = lambda: None

    firmware = types.ModuleType('e2e_support.firmware')
    firmware.from_run_script = lambda: None
    firmware.build = lambda scratch: None

    def spawn(fw, port, args=()):
        _events().append(('spawn', _clock().now))
        return types.SimpleNamespace(ready_at=_clock().now, stop=lambda: None)

    firmware.spawn = spawn

    relay = types.ModuleType('e2e_support.relay')
    relay.PtyRelay = type('PtyRelay', (), {})

    teardown = types.ModuleType('e2e_support.teardown')
    teardown.shutdown = lambda: _events().append(('shutdown',))

    support = types.ModuleType('e2e_support')
    for name, child in (
        ('bridge', bridge),
        ('checks', real_checks),
        ('firmware', firmware),
        ('relay', relay),
        ('teardown', teardown),
    ):
        sys.modules[f'e2e_support.{name}'] = child
        setattr(support, name, child)
    sys.modules['e2e_support'] = support
    sys.modules['launch_testing'] = launch_testing
    sys.modules['launch_testing.markers'] = markers
    sys.modules['rclpy'] = rclpy
    sys.modules['rclpy.qos'] = qos
    sys.modules['builtin_interfaces'] = builtin_interfaces
    sys.modules['builtin_interfaces.msg'] = builtin_interfaces_msg
    sys.modules['diagnostic_msgs'] = diagnostic_msgs
    sys.modules['diagnostic_msgs.msg'] = diagnostic_msgs_msg
    return real_checks


class World:
    """One driven scenario run with its recorded event log."""

    def __init__(self, topics):
        global _current
        self.events = []
        self.nodes = []
        self.clock = FakeClock()
        self.topics = topics
        _current = self

    def load(self, name, path):
        """The scenario module on the stepped clock; pacing is not logic."""
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        clock = self.clock
        module.time = types.SimpleNamespace(monotonic=clock.monotonic, sleep=lambda seconds: None)
        return module

    def drive(self, module):
        global _current
        _current = self
        method = 'test_skewed_mcu_time_stays_within_bound_of_host_time'
        case = module.TestScenario8(method)
        module.TestScenario8.setUpClass()
        try:
            getattr(case, method)()
        finally:
            module.TestScenario8.tearDownClass()


def test_scenario_8_judges_through_the_least_delayed_helper(monkeypatch):
    monkeypatch.delenv('R2S_SYNC_BOUND_NS', raising=False)
    saved = {name: sys.modules.get(name) for name in _STUB_NAMES}
    try:
        _install_stubs()
        world = World(['battery', 'cmd_vel', 'echo_vel', 'log_text', 'time_now'])
        module = world.load('s8_stamp_judge_probe', POSIX_8)
        assert module.HANDSHAKE_TOPICS == world.topics
        module._FIRMWARE = 'fake-firmware'
        module._RELAY = types.SimpleNamespace(mcu_port='/dev/fake', stop=lambda: None)
        assert module.JUDGE_STAMPS == 10
        world.drive(module)
    finally:
        for name, stub in saved.items():
            if stub is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = stub
    judges = [event for event in world.events if event[0] == 'judge']
    assert len(judges) == 1, f'expected one judge call, got {judges!r}'
    _, stamps, rtts, keep = judges[0]
    assert keep == module.JUDGE_STAMPS == 10
    assert len(stamps) >= module.MIN_STAMPS
    assert len(rtts) >= 5
    assert all(isinstance(stamp, tuple) and len(stamp) == 2 for stamp in stamps)
    assert all(stamp_ns == host_ns for stamp_ns, host_ns in stamps)
    assert all(rtt == 1_000_000 for rtt in rtts)
