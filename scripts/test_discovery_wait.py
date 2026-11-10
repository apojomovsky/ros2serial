# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The discovery wait spins the node and reports the last graph state.

Regression test for #515: the rolling Fast DDS legs missed /echo_vel for
the whole 20 s while its siblings stayed visible. The wait polled the graph
without touching the node and named only the missing topics. Now every poll
spins the node once, and the timeout carries the last graph snapshot with
per-topic endpoint counts. These tests drive the real helper with fakes.
"""

import importlib.util
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / 'tests' / 'e2e'

STEP = 0.1

_STUB_NAMES = (
    'launch',
    'launch.actions',
    'launch.launch_description_sources',
    'launch_testing',
    'launch_testing.actions',
    'launch_testing.markers',
    'ament_index_python',
    'ament_index_python.packages',
    'launch_ros',
    'launch_ros.actions',
    'launch_ros.substitutions',
    'rcl_interfaces',
    'rcl_interfaces.srv',
)


class FakeClock:
    """A stepped clock: every spin advances it, so timing stays exact."""

    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FakeNode:
    """A probe node with a scripted graph: each poll pops one snapshot."""

    def __init__(self, graphs, counts, clock):
        self._graphs = [list(graph) for graph in graphs]
        self._counts = dict(counts)
        self._clock = clock
        self.spins = 0

    def get_topic_names_and_types(self):
        if len(self._graphs) > 1:
            return self._graphs.pop(0)
        return self._graphs[0]

    def count_publishers(self, topic):
        return self._counts.get(topic, (0, 0))[0]

    def count_subscribers(self, topic):
        return self._counts.get(topic, (0, 0))[1]

    def spin(self):
        self.spins += 1
        self._clock.advance(STEP)


def _load_real_bridge():
    """The real bridge helper with stubbed launch-time imports.

    bridge.py only needs its launch imports for the launch helpers; the
    discovery wait under test touches none of them. The stubs live only
    while the caller holds them: the caller restores sys.modules after.
    """
    saved = {name: sys.modules.get(name) for name in _STUB_NAMES}

    def factory(**kwargs):
        return type('Stub', (), kwargs)

    launch = types.ModuleType('launch')
    launch_actions = types.ModuleType('launch.actions')
    launch_actions.GroupAction = factory()
    launch_actions.IncludeLaunchDescription = factory()
    launch_sources = types.ModuleType('launch.launch_description_sources')
    launch_sources.PythonLaunchDescriptionSource = factory()
    launch_testing = types.ModuleType('launch_testing')
    testing_actions = types.ModuleType('launch_testing.actions')
    testing_actions.ReadyToTest = factory()
    testing_markers = types.ModuleType('launch_testing.markers')
    testing_markers.keep_alive = lambda func: func
    ament = types.ModuleType('ament_index_python')
    ament_packages = types.ModuleType('ament_index_python.packages')
    ament_packages.get_package_share_directory = lambda name: '/nonexistent'
    ros_actions = types.ModuleType('launch_ros.actions')
    ros_actions.PushRosNamespace = factory()
    ros_actions.SetRemap = factory()
    ros_subs = types.ModuleType('launch_ros.substitutions')
    ros_subs.FindPackageShare = factory()
    ros = types.ModuleType('launch_ros')
    interfaces = types.ModuleType('rcl_interfaces')
    interfaces_srv = types.ModuleType('rcl_interfaces.srv')
    interfaces_srv.GetParameters = factory()

    for name, module in (
        ('launch', launch),
        ('launch.actions', launch_actions),
        ('launch.launch_description_sources', launch_sources),
        ('launch_testing', launch_testing),
        ('launch_testing.actions', testing_actions),
        ('launch_testing.markers', testing_markers),
        ('ament_index_python', ament),
        ('ament_index_python.packages', ament_packages),
        ('launch_ros', ros),
        ('launch_ros.actions', ros_actions),
        ('launch_ros.substitutions', ros_subs),
        ('rcl_interfaces', interfaces),
        ('rcl_interfaces.srv', interfaces_srv),
    ):
        sys.modules[name] = module
    spec = importlib.util.spec_from_file_location(
        'discovery_wait_real_bridge', E2E / 'e2e_support' / 'bridge.py'
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules['discovery_wait_real_bridge'] = module
    spec.loader.exec_module(module)
    return module, saved


def _restore(saved):
    for name, module in saved.items():
        if module is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = module
    sys.modules.pop('discovery_wait_real_bridge', None)


def _run(graphs, counts, wanted, budget, what='echo topics'):
    """Drive the real wait on a scripted graph; return (took, node)."""
    bridge, saved = _load_real_bridge()
    try:
        clock = FakeClock()
        node = FakeNode(graphs, counts, clock)
        took = bridge.wait_for_topics(
            node, wanted, clock.now, budget, what, node.spin, clock=clock.monotonic
        )
        return took, node
    finally:
        _restore(saved)


def test_returns_at_once_when_everything_listed():
    graph = [
        [('/cmd_vel', ['geometry_msgs/msg/Twist']), ('/echo_vel', ['geometry_msgs/msg/Twist'])]
    ]
    took, node = _run(graph, {}, ['/cmd_vel', '/echo_vel'], 20.0)
    assert took == pytest.approx(0.0)
    assert node.spins == 0


def test_spins_until_late_topics_appear():
    partial = [[('/cmd_vel', ['geometry_msgs/msg/Twist'])]]
    full = [
        [('/cmd_vel', ['geometry_msgs/msg/Twist']), ('/echo_vel', ['geometry_msgs/msg/Twist'])]
    ]
    took, node = _run(partial + partial + full, {}, ['/cmd_vel', '/echo_vel'], 20.0)
    assert took == pytest.approx(2 * STEP)
    assert node.spins == 2


def test_timeout_reports_the_last_graph_state():
    graph = [
        ('/cmd_vel', ['geometry_msgs/msg/Twist']),
        ('/rosout', ['rcl_interfaces/msg/Log', 'other/msg/Note']),
    ]
    counts = {'/cmd_vel': (0, 1), '/echo_vel': (0, 0)}
    bridge, saved = _load_real_bridge()
    try:
        clock = FakeClock()
        node = FakeNode([graph], counts, clock)
        with pytest.raises(
            AssertionError, match=r"echo topics missing after 0\.3 s: \['/echo_vel'\]"
        ) as excinfo:
            bridge.wait_for_topics(
                node,
                ['/cmd_vel', '/echo_vel'],
                clock.now,
                0.3,
                'echo topics',
                node.spin,
                clock=clock.monotonic,
            )
    finally:
        _restore(saved)
    assert node.spins == 3
    message = str(excinfo.value)
    assert '/cmd_vel: geometry_msgs/msg/Twist' in message
    assert '/rosout: rcl_interfaces/msg/Log, other/msg/Note' in message
    assert "'/echo_vel': (0, 0)" in message and "'/cmd_vel': (0, 1)" in message


def test_timeout_reports_an_empty_graph():
    bridge, saved = _load_real_bridge()
    try:
        clock = FakeClock()
        node = FakeNode([[]], {}, clock)
        with pytest.raises(
            AssertionError, match=r"echo topics missing after 0\.2 s: \['/cmd_vel', '/echo_vel'\]"
        ) as excinfo:
            bridge.wait_for_topics(
                node,
                ['/cmd_vel', '/echo_vel'],
                clock.now,
                0.2,
                'echo topics',
                node.spin,
                clock=clock.monotonic,
            )
    finally:
        _restore(saved)
    assert node.spins == 2
    assert 'last graph: []' in str(excinfo.value)
