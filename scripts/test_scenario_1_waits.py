"""Scenario 1 stages its waits: handshake on bridge state, then discovery.

Regression test for #411: the discovery budget must run from the completed
handshake, not from the firmware spawn, so a slow handshake cannot spend it.
This drives the posix scenario module with recording fakes and a stepped
clock, pins the wait order, and pins the timestamp the discovery wait starts
from; it needs no ROS.
"""

import importlib.util
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / "tests" / "e2e"
POSIX_CASE = E2E / "test_scenario_1_posix.py"

STEP = 0.25

_current = None


def _events():
    assert _current is not None, "stub ROS called outside a driven scenario"
    return _current.events


def _nodes():
    assert _current is not None, "stub ROS called outside a driven scenario"
    return _current.nodes


def _script():
    assert _current is not None, "stub ROS called outside a driven scenario"
    return _current.next_script


def _clock():
    assert _current is not None, "stub ROS called outside a driven scenario"
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


class FakeNode:
    """A probe node with a scripted spin: each spin steps time, then acts."""

    def __init__(self, events, script, clock):
        self._events = events
        self._subs = {}
        self._script = script
        self._clock = clock

    def create_subscription(self, msg_type, topic, callback, qos):
        self._events.append(("sub", topic))
        self._subs.setdefault(topic, []).append(callback)
        return ("sub", topic)

    def destroy_node(self):
        self._events.append(("destroy_node",))

    def spin_once(self):
        self._clock.advance(STEP)
        if self._script:
            self._script.pop(0)()

    def deliver(self, topic, msg):
        for callback in self._subs.get(topic, []):
            callback(msg)


_STUB_NAMES = (
    "launch_testing",
    "launch_testing.markers",
    "rclpy",
    "diagnostic_msgs",
    "diagnostic_msgs.msg",
    "e2e_support",
    "e2e_support.bridge",
    "e2e_support.checks",
    "e2e_support.firmware",
    "e2e_support.relay",
    "e2e_support.teardown",
)


def _install_stubs():
    """Recording fakes for the ROS imports the scenario module needs.

    Everything is installed here, at test time, so this file never depends
    on which other stub-based test module pytest imported first. Nothing
    here touches the real e2e_support package: the checks helper loads
    straight from its file, keeping the stub package pathless, so a missing
    fake fails loudly instead of importing the real backend.
    """
    spec = importlib.util.spec_from_file_location("s1_waits_real_checks", E2E / "e2e_support" / "checks.py")
    real_checks = importlib.util.module_from_spec(spec)
    sys.modules["s1_waits_real_checks"] = real_checks
    spec.loader.exec_module(real_checks)

    launch_testing = types.ModuleType("launch_testing")
    markers = types.ModuleType("launch_testing.markers")
    markers.keep_alive = lambda func: func
    launch_testing.markers = markers

    rclpy = types.ModuleType("rclpy")

    def create_node(name):
        node = FakeNode(_events(), _script(), _clock())
        _nodes().append(node)
        return node

    rclpy.create_node = create_node
    rclpy.init = lambda: _events().append(("init",))
    rclpy.spin_once = lambda node, timeout_sec=0.0: node.spin_once()

    diagnostic_msgs = types.ModuleType("diagnostic_msgs")
    diagnostic_msgs_msg = types.ModuleType("diagnostic_msgs.msg")
    diagnostic_msgs_msg.DiagnosticArray = type("DiagnosticArray", (), {})
    diagnostic_msgs.msg = diagnostic_msgs_msg

    bridge = types.ModuleType("e2e_support.bridge")
    bridge.HANDSHAKE_TIMEOUT = 20.0
    bridge.wait_for_bridge = lambda node, service="", timeout=0.0: _events().append(("wait_for_bridge",))
    bridge.wait_for_topics = lambda node, wanted, since, budget, what: _events().append(
        ("wait_for_topics", tuple(wanted), since, budget)
    )
    bridge.launch_bridge_only = lambda *args, **kwargs: None
    bridge.example_params_file = lambda: None

    firmware = types.ModuleType("e2e_support.firmware")
    firmware.from_run_script = lambda: None

    def spawn(fw, port, args=()):
        _events().append(("spawn", _clock().now))
        return types.SimpleNamespace(pty=port, ready_at=_clock().now)

    firmware.spawn = spawn

    relay = types.ModuleType("e2e_support.relay")
    relay.PtyRelay = type("PtyRelay", (), {})

    teardown = types.ModuleType("e2e_support.teardown")
    teardown.shutdown = lambda: _events().append(("shutdown",))

    support = types.ModuleType("e2e_support")
    for name, child in (
        ("bridge", bridge),
        ("checks", real_checks),
        ("firmware", firmware),
        ("relay", relay),
        ("teardown", teardown),
    ):
        sys.modules[f"e2e_support.{name}"] = child
        setattr(support, name, child)
    sys.modules["e2e_support"] = support
    sys.modules["launch_testing"] = launch_testing
    sys.modules["launch_testing.markers"] = markers
    sys.modules["rclpy"] = rclpy
    sys.modules["diagnostic_msgs"] = diagnostic_msgs
    sys.modules["diagnostic_msgs.msg"] = diagnostic_msgs_msg
    return real_checks


class World:
    """One driven scenario run with its recorded event log."""

    def __init__(self):
        global _current
        self.events = []
        self.nodes = []
        self.next_script = []
        self.clock = FakeClock()
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
        case = module.TestScenario1("test_handshake_announces_topics_within_3s_of_hello")
        module.TestScenario1.setUpClass()
        try:
            case.test_handshake_announces_topics_within_3s_of_hello()
        finally:
            module.TestScenario1.tearDownClass()


def _connected(names):
    return FakeDiagnostics("connected", [(f"topic {name} status", "ok") for name in names])


def _handshake_script(world):
    """Spins that connect the link slowly: silence, then handshake, then ok."""

    def deliver(topic, make):
        node = world.nodes[-1]
        node.deliver(topic, make())

    return [
        lambda: deliver("/diagnostics", lambda: FakeDiagnostics("handshaking")),
        lambda: deliver("/diagnostics", lambda: FakeDiagnostics("handshaking")),
        lambda: deliver("/diagnostics", lambda: _connected(world.module.HANDSHAKE_TOPICS)),
    ]


def test_posix_discovers_on_its_own_budget_after_the_handshake():
    saved = {name: sys.modules.get(name) for name in _STUB_NAMES}
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s1_posix_waits_probe", POSIX_CASE)
        world.module = module
        module._FIRMWARE = "fake-firmware"
        module._RELAY = types.SimpleNamespace(mcu_port="/dev/fake")
        assert module.TOPIC_BUDGET == 3.0
        assert set(module.ANNOUNCED) == {"/battery", "/cmd_vel", "/echo_vel", "/log_text", "/time_now"}
        assert module.HANDSHAKE_TOPICS == [name.removeprefix("/") for name in module.ANNOUNCED]
        world.next_script.extend(_handshake_script(world))
        world.drive(module)
    finally:
        for name, stub in saved.items():
            if stub is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = stub
    discovery = next(event for event in world.events if event[0] == "wait_for_topics")
    assert discovery[1] == tuple(module.ANNOUNCED)
    assert discovery[3] == real_checks.DISCOVERY_TIMEOUT
    spawn = next(event for event in world.events if event[0] == "spawn")
    ready_at = spawn[1]
    since = discovery[2]
    assert since == ready_at + 2 * STEP
    diag = world.events.index(("sub", "/diagnostics"))
    assert diag < world.events.index(spawn) < world.events.index(discovery)
