"""Scenario 3 waits for handshake and discovery before priming the echo.

Regression test for #365: the priming loop used to conflate the serial
handshake, DDS discovery and the first echo into one budget, so a stall
in any layer failed as "no echo flow". Both scenario 3 backends now wait
for the handshake, then for discovery of the echo topics, and only then
prime. These tests drive both scenario modules with recording fakes and
pin that order; they need no ROS.
"""

import importlib.util
import sys
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / 'tests' / 'e2e'
POSIX_CASE = E2E / 'test_scenario_3_posix.py'
AVR_CASE = E2E / 'test_scenario_3_avr.py'

_current = None


def _events():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.events


def _nodes():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.nodes


def _script():
    assert _current is not None, 'stub ROS called outside a driven scenario'
    return _current.next_script


class FakeQoS:
    def __init__(self, depth=None, reliability=None):
        self.depth = depth
        self.reliability = reliability


class FakeVector:
    def __init__(self):
        self.x = self.y = self.z = 0.0


class FakeTwist:
    def __init__(self):
        self.linear = FakeVector()
        self.angular = FakeVector()


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


class FakePublisher:
    def __init__(self, events, topic):
        self._events = events
        self._topic = topic

    def publish(self, msg):
        self._events.append(('publish', self._topic))


class FakeNode:
    """A probe node with a scripted spin: each spin runs one action."""

    def __init__(self, events, script):
        self._events = events
        self._subs = {}
        self._script = script

    def create_subscription(self, msg_type, topic, callback, qos):
        self._events.append(('sub', topic))
        self._subs.setdefault(topic, []).append(callback)
        return ('sub', topic)

    def create_publisher(self, msg_type, topic, qos):
        self._events.append(('pub', topic))
        return FakePublisher(self._events, topic)

    def destroy_node(self):
        self._events.append(('destroy_node',))

    def spin_once(self):
        if self._script:
            self._script.pop(0)()

    def deliver(self, topic, msg):
        for callback in self._subs.get(topic, []):
            callback(msg)


def _install_stubs():
    """Recording fakes for the ROS imports the scenario modules need.

    Everything is installed here, at import time, so every test module in
    this directory sees one complete set no matter the collection order.
    Nothing here touches the real e2e_support package: the checks helper
    loads straight from its file, keeping the stub package pathless, so
    a missing fake fails loudly instead of importing the real backend.
    """
    for _name in (
        'launch_testing',
        'launch_testing.markers',
        'rclpy',
        'rclpy.qos',
        'geometry_msgs',
        'geometry_msgs.msg',
        'diagnostic_msgs',
        'diagnostic_msgs.msg',
        'e2e_support',
        'e2e_support.avr',
        'e2e_support.bridge',
        'e2e_support.checks',
        'e2e_support.firmware',
        'e2e_support.relay',
        'e2e_support.teardown',
    ):
        sys.modules.pop(_name, None)

    spec = importlib.util.spec_from_file_location(
        's3_waits_real_checks', E2E / 'e2e_support' / 'checks.py'
    )
    real_checks = importlib.util.module_from_spec(spec)
    sys.modules['s3_waits_real_checks'] = real_checks
    spec.loader.exec_module(real_checks)

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
        node = FakeNode(_events(), _script())
        _nodes().append(node)
        return node

    rclpy.create_node = create_node
    rclpy.init = lambda: _events().append(('init',))
    rclpy.spin_once = lambda node, timeout_sec=0.0: node.spin_once()

    geometry_msgs = types.ModuleType('geometry_msgs')
    geometry_msgs_msg = types.ModuleType('geometry_msgs.msg')
    geometry_msgs_msg.Twist = FakeTwist
    geometry_msgs.msg = geometry_msgs_msg

    diagnostic_msgs = types.ModuleType('diagnostic_msgs')
    diagnostic_msgs_msg = types.ModuleType('diagnostic_msgs.msg')
    diagnostic_msgs_msg.DiagnosticArray = type('DiagnosticArray', (), {})
    diagnostic_msgs.msg = diagnostic_msgs_msg

    bridge = types.ModuleType('e2e_support.bridge')
    bridge.HANDSHAKE_TIMEOUT = 20.0
    bridge.DISCOVERY_SPIN = 0.1
    bridge.wait_for_bridge = lambda node, service='', timeout=0.0: _events().append(
        ('wait_for_bridge',)
    )

    def wait_for_topics(node, wanted, since, budget, what, spin):
        assert callable(spin), 'the scenario must pass a spin callback'
        _events().append(('wait_for_topics', tuple(wanted), budget))

    bridge.wait_for_topics = wait_for_topics
    bridge.launch_bridge_only = lambda *args, **kwargs: None
    bridge.example_params_file = lambda: None

    firmware = types.ModuleType('e2e_support.firmware')
    firmware.from_run_script = lambda: None

    def spawn(fw, port, args=()):
        _events().append(('spawn',))
        return types.SimpleNamespace(pty=port, ready_at=time.monotonic())

    firmware.spawn = spawn

    avr = types.ModuleType('e2e_support.avr')
    avr.spawn = lambda board='uno': types.SimpleNamespace(
        pty='/dev/fake', ready_at=None, stop=lambda: 0
    )

    relay = types.ModuleType('e2e_support.relay')
    relay.PtyRelay = type('PtyRelay', (), {})

    teardown = types.ModuleType('e2e_support.teardown')
    teardown.shutdown = lambda: _events().append(('shutdown',))

    support = types.ModuleType('e2e_support')
    for name, child in (
        ('bridge', bridge),
        ('checks', real_checks),
        ('firmware', firmware),
        ('avr', avr),
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
    sys.modules['geometry_msgs'] = geometry_msgs
    sys.modules['geometry_msgs.msg'] = geometry_msgs_msg
    sys.modules['diagnostic_msgs'] = diagnostic_msgs
    sys.modules['diagnostic_msgs.msg'] = diagnostic_msgs_msg
    return real_checks


real_checks = _install_stubs()


class World:
    """One driven scenario run with its recorded event log."""

    def __init__(self):
        global _current
        self.events = []
        self.nodes = []
        self.next_script = []
        _current = self

    def load(self, name, path):
        """The scenario module with sleeps patched out; pacing is not logic."""
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        module.time = types.SimpleNamespace(monotonic=time.monotonic, sleep=lambda seconds: None)
        return module

    def drive(self, module):
        global _current
        _current = self
        case = module.TestScenario3('test_published_twists_come_back_on_the_echo_topic')
        module.TestScenario3.setUpClass()
        try:
            case.test_published_twists_come_back_on_the_echo_topic()
        finally:
            module.TestScenario3.tearDownClass()


def _copy(source):
    msg = FakeTwist()
    msg.linear.x, msg.linear.y, msg.linear.z = source.linear.x, source.linear.y, source.linear.z
    msg.angular.x, msg.angular.y, msg.angular.z = (
        source.angular.x,
        source.angular.y,
        source.angular.z,
    )
    return msg


def _connected(names):
    return FakeDiagnostics(
        'connected', [(f'topic {16 + i} {name} status', 'ok') for i, name in enumerate(names)]
    )


def _prime_script(world, module):
    """Spins that bring the echo path up slowly, then deliver every echo."""

    def deliver(topic, make):
        node = world.nodes[-1]
        node.deliver(topic, make())

    return [
        lambda: None,
        lambda: deliver('/diagnostics', lambda: FakeDiagnostics('handshaking')),
        lambda: deliver('/diagnostics', lambda: FakeDiagnostics('handshaking')),
        lambda: deliver('/diagnostics', lambda: _connected(module.HANDSHAKE_TOPICS)),
        lambda: None,
        lambda: deliver('echo_vel', lambda: _copy(module.PRIME)),
        lambda: deliver('echo_vel', lambda: _copy(module.SENT[0])),
        lambda: deliver('echo_vel', lambda: _copy(module.SENT[1])),
        lambda: deliver('echo_vel', lambda: _copy(module.SENT[2])),
        lambda: deliver('echo_vel', lambda: _copy(module.SENT[3])),
        lambda: deliver('echo_vel', lambda: _copy(module.SENT[4])),
    ]


def test_posix_primes_only_after_handshake_and_discovery():
    world = World()
    module = world.load('s3_posix_waits_probe', POSIX_CASE)
    module._FIRMWARE = 'fake-firmware'
    module._RELAY = types.SimpleNamespace(mcu_port='/dev/fake')
    assert module.TOPIC_BUDGET == 3.0
    assert 'cmd_vel' in module.HANDSHAKE_TOPICS
    assert 'echo_vel' in module.HANDSHAKE_TOPICS
    assert set(module.ECHO_TOPICS) == {'/cmd_vel', '/echo_vel'}
    world.next_script.extend(_prime_script(world, module))
    world.drive(module)
    discovery = ('wait_for_topics', tuple(module.ECHO_TOPICS), real_checks.DISCOVERY_TIMEOUT)
    assert ('sub', '/diagnostics') in world.events
    assert discovery in world.events
    diag = world.events.index(('sub', '/diagnostics'))
    spawn = world.events.index(('spawn',))
    found = world.events.index(discovery)
    echo_sub = world.events.index(('sub', 'echo_vel'))
    first_pub = next(i for i, event in enumerate(world.events) if event[0] == 'publish')
    assert diag < spawn < found < echo_sub < first_pub


def test_avr_primes_only_after_handshake_and_discovery():
    world = World()
    module = world.load('s3_avr_waits_probe', AVR_CASE)
    assert module.TOPIC_BUDGET == 3.0
    assert getattr(module, 'HANDSHAKE_TOPICS', None) is not None, (
        'scenario 3 AVR has no handshake wait'
    )
    assert 'cmd_vel' in module.HANDSHAKE_TOPICS
    assert 'echo_vel' in module.HANDSHAKE_TOPICS
    assert set(module.ECHO_TOPICS) == {'/cmd_vel', '/echo_vel'}
    module._SPAWNED = sys.modules['e2e_support.avr'].spawn('uno')

    def deliver(topic, make):
        node = world.nodes[-1]
        node.deliver(topic, make())

    world.next_script.extend(
        [
            lambda: None,
            lambda: deliver('/diagnostics', lambda: FakeDiagnostics('handshaking')),
            lambda: deliver('/diagnostics', lambda: FakeDiagnostics('handshaking')),
            lambda: deliver('/diagnostics', lambda: _connected(module.HANDSHAKE_TOPICS)),
            lambda: deliver('echo_vel', lambda: _copy(module.PRIME)),
            lambda: deliver('echo_vel', lambda: _copy(module.SENT[0])),
            lambda: deliver('echo_vel', lambda: _copy(module.SENT[1])),
            lambda: deliver('echo_vel', lambda: _copy(module.SENT[2])),
            lambda: deliver('echo_vel', lambda: _copy(module.SENT[3])),
            lambda: deliver('echo_vel', lambda: _copy(module.SENT[4])),
        ]
    )
    world.drive(module)
    discovery = ('wait_for_topics', tuple(module.ECHO_TOPICS), real_checks.DISCOVERY_TIMEOUT)
    assert ('sub', '/diagnostics') in world.events, 'scenario 3 AVR never reads the handshake'
    assert discovery in world.events
    diag = world.events.index(('sub', '/diagnostics'))
    found = world.events.index(discovery)
    echo_sub = world.events.index(('sub', 'echo_vel'))
    first_pub = next(i for i, event in enumerate(world.events) if event[0] == 'publish')
    assert diag < found < echo_sub < first_pub
