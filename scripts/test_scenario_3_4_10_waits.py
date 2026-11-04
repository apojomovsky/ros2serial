"""Discovery waits run from the completed handshake in scenarios 3, 4 and 10.

Regression tests for #412: the discovery budget must run from the completed
handshake, not from the firmware spawn, so a slow handshake cannot spend it.
Each test drives its scenario module with recording fakes and a stepped
clock, pins the wait order, and pins the timestamp the discovery wait starts
from; they need no ROS. The avr scenario 1 test pins the same staging from
bridge-up, since the boot HELLO predates the bridge there.
"""

import importlib.util
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / "tests" / "e2e"
POSIX_3 = E2E / "test_scenario_3_posix.py"
POSIX_4 = E2E / "test_scenario_4_posix.py"
POSIX_10 = E2E / "test_scenario_10_posix.py"
POSIX_10F = E2E / "test_scenario_10_framing_posix.py"
AVR_1 = E2E / "test_scenario_1_avr.py"

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


class FakeVector:
    def __init__(self):
        self.x = self.y = self.z = 0.0


class FakeTwist:
    def __init__(self):
        self.linear = FakeVector()
        self.angular = FakeVector()


class FakeQoS:
    def __init__(self, depth=None, reliability=None):
        self.depth = depth
        self.reliability = reliability


class FakePublisher:
    def __init__(self, events, topic):
        self._events = events
        self._topic = topic

    def publish(self, msg):
        self._events.append(("publish", self._topic))


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

    def create_publisher(self, msg_type, topic, qos):
        self._events.append(("pub", topic))
        return FakePublisher(self._events, topic)

    def get_topic_names_and_types(self):
        return [(name, "type") for name in ("/e2e_ns/cell_voltage", "/e2e_ns/steer", "/time_now")]

    def destroy_node(self):
        self._events.append(("destroy_node",))

    def spin_once(self):
        self._clock.advance(STEP)
        if self._script:
            self._script.pop(0)()

    def deliver(self, topic, msg):
        for callback in self._subs.get(topic, []):
            callback(msg)


class _FakeProxy:
    """Fixed touches once faults are on; the frame count always advances."""

    def __init__(self, host_touched=0, user_touched=0):
        self._calls = 0
        self._faulted = False
        self._host_touched = host_touched
        self._user_touched = user_touched
        self.mcu_port = "/dev/fake"

    def start(self):
        return None

    def stop(self):
        return None

    def set_faults(self, **kwargs):
        if kwargs:
            self._faulted = True

    def counts(self):
        self._calls += 1
        touched = self._faulted
        return {
            "host_to_mcu": types.SimpleNamespace(
                frames=self._calls, frames_touched=self._host_touched if touched else 0
            ),
            "mcu_to_host": types.SimpleNamespace(
                frames=self._calls, frames_touched=0, user_frames_touched=self._user_touched if touched else 0
            ),
        }


_STUB_NAMES = (
    "launch_testing",
    "launch_testing.markers",
    "rclpy",
    "rclpy.qos",
    "geometry_msgs",
    "geometry_msgs.msg",
    "std_msgs",
    "std_msgs.msg",
    "diagnostic_msgs",
    "diagnostic_msgs.msg",
    "e2e_support",
    "e2e_support.avr",
    "e2e_support.bridge",
    "e2e_support.checks",
    "e2e_support.firmware",
    "e2e_support.fault_proxy",
    "e2e_support.relay",
    "e2e_support.teardown",
)


def _install_stubs():
    """Recording fakes for the ROS imports the scenario modules need.

    Everything is installed here, at test time, so this file never depends
    on which other stub-based test module pytest imported first. Nothing
    here touches the real e2e_support package: the checks helper loads
    straight from its file, keeping the stub package pathless, so a missing
    fake fails loudly instead of importing the real backend.
    """
    spec = importlib.util.spec_from_file_location("s3410_waits_real_checks", E2E / "e2e_support" / "checks.py")
    real_checks = importlib.util.module_from_spec(spec)
    sys.modules["s3410_waits_real_checks"] = real_checks
    spec.loader.exec_module(real_checks)
    # The helper keeps its file and its real time module: the scenario
    # passes its own time.monotonic as the clock, so the driven run needs
    # no patching of the helper's globals on any host uptime.

    launch_testing = types.ModuleType("launch_testing")
    markers = types.ModuleType("launch_testing.markers")
    markers.keep_alive = lambda func: func
    launch_testing.markers = markers

    rclpy = types.ModuleType("rclpy")
    qos = types.ModuleType("rclpy.qos")
    qos.QoSProfile = FakeQoS
    qos.ReliabilityPolicy = type("ReliabilityPolicy", (), {"BEST_EFFORT": 1})
    rclpy.qos = qos

    def create_node(name):
        node = FakeNode(_events(), _script(), _clock())
        _nodes().append(node)
        return node

    rclpy.create_node = create_node
    rclpy.init = lambda: _events().append(("init",))
    rclpy.spin_once = lambda node, timeout_sec=0.0: node.spin_once()

    geometry_msgs = types.ModuleType("geometry_msgs")
    geometry_msgs_msg = types.ModuleType("geometry_msgs.msg")
    geometry_msgs_msg.Twist = FakeTwist
    geometry_msgs.msg = geometry_msgs_msg

    std_msgs = types.ModuleType("std_msgs")
    std_msgs_msg = types.ModuleType("std_msgs.msg")
    std_msgs_msg.Float32 = type("Float32", (), {})
    std_msgs.msg = std_msgs_msg

    diagnostic_msgs = types.ModuleType("diagnostic_msgs")
    diagnostic_msgs_msg = types.ModuleType("diagnostic_msgs.msg")
    diagnostic_msgs_msg.DiagnosticArray = type("DiagnosticArray", (), {})
    diagnostic_msgs.msg = diagnostic_msgs_msg

    bridge = types.ModuleType("e2e_support.bridge")
    bridge.HANDSHAKE_TIMEOUT = 20.0

    def wait_for_topics(node, wanted, since, budget, what):
        _events().append(("wait_for_topics", tuple(wanted), since, budget))
        return 0.0

    bridge.wait_for_bridge = lambda node, service="", timeout=0.0: _events().append(("wait_for_bridge",))
    bridge.wait_for_topics = wait_for_topics
    bridge.launch_bridge_only = lambda *args, **kwargs: None
    bridge.example_params_file = lambda: None

    firmware = types.ModuleType("e2e_support.firmware")
    firmware.from_run_script = lambda: None

    def spawn(fw, port, args=()):
        _events().append(("spawn", _clock().now))
        proc = types.SimpleNamespace(poll=lambda: None)
        return types.SimpleNamespace(pty=port, ready_at=_clock().now, proc=proc, stop=lambda: None)

    firmware.spawn = spawn

    avr = types.ModuleType("e2e_support.avr")
    avr.spawn = lambda board="uno": types.SimpleNamespace(pty="/dev/fake", ready_at=None, stop=lambda: 0)

    fault_proxy = types.ModuleType("e2e_support.fault_proxy")
    fault_proxy.FaultProxy = type("FaultProxy", (), {})

    relay = types.ModuleType("e2e_support.relay")
    relay.PtyRelay = type("PtyRelay", (), {})

    teardown = types.ModuleType("e2e_support.teardown")
    teardown.shutdown = lambda: _events().append(("shutdown",))

    support = types.ModuleType("e2e_support")
    for name, child in (
        ("bridge", bridge),
        ("checks", real_checks),
        ("firmware", firmware),
        ("avr", avr),
        ("fault_proxy", fault_proxy),
        ("relay", relay),
        ("teardown", teardown),
    ):
        sys.modules[f"e2e_support.{name}"] = child
        setattr(support, name, child)
    sys.modules["e2e_support"] = support
    sys.modules["launch_testing"] = launch_testing
    sys.modules["launch_testing.markers"] = markers
    sys.modules["rclpy"] = rclpy
    sys.modules["rclpy.qos"] = qos
    sys.modules["geometry_msgs"] = geometry_msgs
    sys.modules["geometry_msgs.msg"] = geometry_msgs_msg
    sys.modules["std_msgs"] = std_msgs
    sys.modules["std_msgs.msg"] = std_msgs_msg
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

    def drive(self, module, cls_name, method_name):
        global _current
        _current = self
        case = getattr(module, cls_name)(method_name)
        getattr(module, cls_name).setUpClass()
        try:
            getattr(case, method_name)()
        finally:
            getattr(module, cls_name).tearDownClass()


def _deliver(world, topic, msg):
    world.nodes[-1].deliver(topic, msg)


def _connected(names):
    return FakeDiagnostics("connected", [(f"topic {name} status", "ok") for name in names])


def _copy_twist(source):
    msg = FakeTwist()
    msg.linear.x, msg.linear.y, msg.linear.z = source.linear.x, source.linear.y, source.linear.z
    msg.angular.x, msg.angular.y, msg.angular.z = source.angular.x, source.angular.y, source.angular.z
    return msg


def _handshake_script(world, names):
    """Silence, then handshake: the discovery wait must start past the spawn."""

    def deliver(topic, make):
        _deliver(world, topic, make())

    script = [lambda: deliver("/diagnostics", lambda: FakeDiagnostics("handshaking"))]
    script.append(lambda: deliver("/diagnostics", lambda: FakeDiagnostics("handshaking")))
    script.append(lambda: deliver("/diagnostics", lambda: _connected(names)))
    return script


def _save_stubs():
    return {name: sys.modules.get(name) for name in _STUB_NAMES}


def _restore_stubs(saved):
    for name, stub in saved.items():
        if stub is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = stub


def _discovery(world):
    return next(event for event in world.events if event[0] == "wait_for_topics")


def test_posix_3_discovers_on_its_own_budget_after_the_handshake():
    saved = _save_stubs()
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s3410_posix_3_probe", POSIX_3)
        module._FIRMWARE = "fake-firmware"
        module._RELAY = types.SimpleNamespace(mcu_port="/dev/fake", stop=lambda: None)
        assert module.TOPIC_BUDGET == 3.0
        assert set(module.ECHO_TOPICS) == {"/cmd_vel", "/echo_vel"}
        world.next_script.extend(_handshake_script(world, module.HANDSHAKE_TOPICS))
        echoes = [_copy_twist(module.PRIME)] + [_copy_twist(sent) for sent in module.SENT]
        for echo in echoes:
            world.next_script.append(lambda echo=echo: _deliver(world, "echo_vel", echo))
        world.drive(module, "TestScenario3", "test_published_twists_come_back_on_the_echo_topic")
    finally:
        _restore_stubs(saved)
    discovery = _discovery(world)
    assert discovery[1] == tuple(module.ECHO_TOPICS)
    assert discovery[3] == real_checks.DISCOVERY_TIMEOUT
    spawn = next(event for event in world.events if event[0] == "spawn")
    assert discovery[2] == spawn[1] + 2 * STEP
    diag = world.events.index(("sub", "/diagnostics"))
    assert diag < world.events.index(("spawn", spawn[1])) < world.events.index(discovery)
    echo_sub = world.events.index(("sub", "echo_vel"))
    first_pub = next(i for i, event in enumerate(world.events) if event[0] == "publish")
    assert world.events.index(discovery) < echo_sub < first_pub


def test_posix_4_discovers_on_its_own_budget_after_the_handshake():
    saved = _save_stubs()
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s3410_posix_4_probe", POSIX_4)
        module._FIRMWARE = "fake-firmware"
        module._RELAY = types.SimpleNamespace(mcu_port="/dev/fake", stop=lambda: None)
        assert module.TOPIC_BUDGET == 3.0
        world.next_script.extend(_handshake_script(world, module.HANDSHAKE_TOPICS))
        world.drive(module, "TestScenario4", "test_remapping_and_namespace_apply_to_mcu_topics")
    finally:
        _restore_stubs(saved)
    discovery = _discovery(world)
    assert discovery[1] == tuple(module.WANTED)
    assert discovery[3] == real_checks.DISCOVERY_TIMEOUT
    spawn = next(event for event in world.events if event[0] == "spawn")
    assert discovery[2] == spawn[1] + 2 * STEP
    diag = world.events.index(("sub", "/diagnostics"))
    assert diag < world.events.index(("spawn", spawn[1])) < world.events.index(discovery)


def _diag_values(dropped, gaps):
    return {
        "resets": "0",
        "mcu dropped frames": str(dropped),
        "topic battery seq gaps": str(gaps),
        "topic battery direction": "device to host",
    }


def _drive_10(world, module, method, framing):
    """The full scenario 10 flow with a consistent proxy and battery stream."""
    module._FIRMWARE = "fake-firmware"
    module._PROXY = _FakeProxy(host_touched=4 if framing else 0, user_touched=3 if framing else 0)
    base = FakeDiagnostics("connected", list(_diag_values(0, 0).items()))
    final = FakeDiagnostics("connected", list(_diag_values(1 if framing else 0, 3 if framing else 0).items()))
    handshaking = FakeDiagnostics("handshaking")
    handshake_done = _connected(module.HANDSHAKE_TOPICS)
    state = {"next": 0, "skips": {50, 150, 250} if framing else set()}

    def spin_flow(diag, values=8):
        for _ in range(values):
            while state["next"] in state["skips"]:
                state["next"] += 1
            value = state["next"]
            state["next"] += 1
            _deliver(world, "battery", types.SimpleNamespace(data=value))
        _deliver(world, "/diagnostics", diag)

    world.next_script.append(lambda: _deliver(world, "/diagnostics", handshaking))
    world.next_script.append(lambda: _deliver(world, "/diagnostics", handshaking))
    world.next_script.append(lambda: _deliver(world, "/diagnostics", handshaking))
    world.next_script.append(lambda: _deliver(world, "/diagnostics", handshake_done))
    world.next_script.append(lambda: spin_flow(base))
    world.next_script.append(lambda: spin_flow(base))
    for _ in range(60):
        world.next_script.append(lambda: spin_flow(final))
    for _ in range(4):
        world.next_script.append(lambda: spin_flow(final))
    for _ in range(24):
        world.next_script.append(lambda: spin_flow(final))
    for _ in range(3):
        world.next_script.append(lambda: spin_flow(final))
    world.drive(module, "TestScenario10Framing" if framing else "TestScenario10", method)


def _check_10_anchor(world, module, real_checks, spins):
    discovery = _discovery(world)
    assert discovery[1] == tuple(module.BATTERY_TOPICS)
    assert discovery[3] == real_checks.DISCOVERY_TIMEOUT
    spawn = next(event for event in world.events if event[0] == "spawn")
    assert discovery[2] == spawn[1] + spins * STEP
    diag = world.events.index(("sub", "/diagnostics"))
    assert diag < world.events.index(("spawn", spawn[1])) < world.events.index(discovery)


def test_posix_10_discovers_on_its_own_budget_after_the_handshake():
    saved = _save_stubs()
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s3410_posix_10_probe", POSIX_10)
        assert module.TOPIC_BUDGET == 3.0
        _drive_10(world, module, "test_corruption_costs_exactly_the_touched_frames_then_recovers", False)
    finally:
        _restore_stubs(saved)
    _check_10_anchor(world, module, real_checks, 3)


def test_posix_10_framing_discovers_on_its_own_budget_after_the_handshake():
    saved = _save_stubs()
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s3410_posix_10f_probe", POSIX_10F)
        assert module.TOPIC_BUDGET == 3.0
        _drive_10(world, module, "test_framing_faults_cost_a_bound_then_recover", True)
    finally:
        _restore_stubs(saved)
    _check_10_anchor(world, module, real_checks, 3)


def test_avr_1_discovers_on_its_own_budget_after_the_handshake():
    saved = _save_stubs()
    real_checks = _install_stubs()
    try:
        world = World()
        module = world.load("s3410_avr_1_probe", AVR_1)
        module._SPAWNED = sys.modules["e2e_support.avr"].spawn("uno")
        assert module.TOPIC_BUDGET == 3.0
        assert set(module.ANNOUNCED) == {"/battery", "/cmd_vel", "/echo_vel", "/log_text"}
        names = [name.removeprefix("/") for name in module.ANNOUNCED]
        world.next_script.extend(_handshake_script(world, names))
        world.drive(module, "TestScenario1", "test_handshake_announces_topics_within_3s_of_hello")
    finally:
        _restore_stubs(saved)
    discovery = _discovery(world)
    assert discovery[1] == tuple(module.ANNOUNCED)
    bridge_up = module._SPAWNED.ready_at
    assert discovery[2] == bridge_up + 2 * STEP
    assert discovery[3] == real_checks.DISCOVERY_TIMEOUT
    assert world.events.index(("wait_for_bridge",)) < world.events.index(("sub", "/diagnostics"))
    assert world.events.index(("sub", "/diagnostics")) < world.events.index(discovery)
