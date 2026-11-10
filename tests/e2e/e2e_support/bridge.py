# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The bridge under its own launch file, as the scenarios need it.

Every scenario drives the bridge through the installed launch file with its
documented arguments, never by spelling the node itself: the launch file and
the example parameters stay the single description of how the bridge runs.
"""

import time
from pathlib import Path

import launch
import launch_testing.actions
from ament_index_python.packages import get_package_share_directory
from launch.actions import GroupAction
from launch.actions import IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch_ros.actions import PushRosNamespace
from launch_ros.actions import SetRemap
from launch_ros.substitutions import FindPackageShare
from rcl_interfaces.srv import GetParameters

HANDSHAKE_TIMEOUT = 20.0

# One discovery poll lets the node process graph events for this long: the
# spin pumps whatever local discovery work the RMW needs, instead of the
# wait sleeping through its budget the way it did before #515.
DISCOVERY_SPIN = 0.1


def example_params_file():
    """Return the installed example parameter file."""
    path = Path(get_package_share_directory('r2s_bridge')) / 'config' / 'r2s_bridge.params.yaml'
    assert path.is_file(), 'example params file missing from the install: ' + str(path)
    return path


def include_bridge(port, params_file, log_level='info', mcu_log_level=''):
    """Include the bridge launch file with the scenario's port, file and log levels."""
    return IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            [FindPackageShare('r2s_bridge'), '/launch/r2s_bridge.launch.py']
        ),
        launch_arguments={
            'port': port,
            'params_file': str(params_file),
            'log_level': log_level,
            'mcu_log_level': mcu_log_level,
        }.items(),
    )


def scoped_include_bridge(
    port, params_file, namespace=None, remappings=(), log_level='info', mcu_log_level=''
):
    """Include the bridge under standard scoping actions for scenario 4.

    PushRosNamespace and SetRemap are the user's own tools for placing
    topics; the scenario proves they reach the MCU's topics through them.
    """
    actions = [include_bridge(port, params_file, log_level, mcu_log_level)]
    if remappings:
        actions = [SetRemap(src, dst) for src, dst in remappings] + actions
    if namespace:
        actions = [PushRosNamespace(namespace)] + actions
    if len(actions) == 1:
        return actions[0]
    return GroupAction(actions)


def launch_bridge_only(
    port, params_file, namespace=None, remappings=(), log_level='info', mcu_log_level=''
):
    """A launch description with the bridge and nothing else.

    The scenarios spawn the firmware themselves once the bridge is up, so
    the 3 s budget of scenario 1 measures the handshake, not node startup.
    """
    return launch.LaunchDescription(
        [
            scoped_include_bridge(
                port, params_file, namespace, remappings, log_level, mcu_log_level
            ),
            launch_testing.actions.ReadyToTest(),
        ]
    )


def wait_for_bridge(node, service='/r2s_bridge/get_parameters', timeout=HANDSHAKE_TIMEOUT):
    """Block until the bridge answers its parameter service, proving it is up."""
    client = node.create_client(GetParameters, service)
    try:
        assert client.wait_for_service(timeout_sec=timeout), (
            'bridge parameter service never appeared'
        )
    finally:
        node.destroy_client(client)


def wait_for_topics(node, wanted, since, budget, what, spin, clock=time.monotonic):
    """Block until every wanted topic is listed, within budget of since.

    spin runs the node's callbacks once per poll, so the wait drives
    discovery instead of sleeping through it. clock reads the clock since
    came from, so driven tests with a stepped clock measure fake seconds.
    The timeout carries the last full graph snapshot with per-topic
    endpoint counts, so the next miss says what the probe saw. Returns
    the seconds from since.
    """
    while True:
        names_and_types = node.get_topic_names_and_types()
        names = {name for name, _ in names_and_types}
        missing = set(wanted) - names
        if not missing:
            return clock() - since
        graph = sorted(f'{name}: {", ".join(types)}' for name, types in names_and_types)
        counts = {
            topic: (node.count_publishers(topic), node.count_subscribers(topic))
            for topic in wanted
        }
        took = clock() - since
        assert took < budget, (
            f'{what} missing after {budget:.1f} s: {sorted(missing)}; '
            f'last graph: {graph}; endpoint counts (publishers, subscribers): {counts}'
        )
        spin()
