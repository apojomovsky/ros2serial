"""The bridge under its own launch file, as the scenarios need it.

Every scenario drives the bridge through the installed launch file with its
documented arguments, never by spelling the node itself: the launch file and
the example parameters stay the single description of how the bridge runs.
"""

import time
from pathlib import Path

import launch
from ament_index_python.packages import get_package_share_directory
from launch.actions import GroupAction, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch_ros.actions import PushRosNamespace, SetRemap
from launch_ros.substitutions import FindPackageShare
from rcl_interfaces.srv import GetParameters

HANDSHAKE_TIMEOUT = 20.0


def example_params_file():
    """Return the installed example parameter file."""
    path = Path(get_package_share_directory("r2s_bridge")) / "config" / "r2s_bridge.params.yaml"
    assert path.is_file(), "example params file missing from the install: " + str(path)
    return path


def include_bridge(port, params_file):
    """Include the bridge launch file with the scenario's port and file."""
    return IncludeLaunchDescription(
        PythonLaunchDescriptionSource([FindPackageShare("r2s_bridge"), "/launch/r2s_bridge.launch.py"]),
        launch_arguments={"port": port, "params_file": str(params_file)}.items(),
    )


def scoped_include_bridge(port, params_file, namespace=None, remappings=()):
    """Include the bridge under standard scoping actions for scenario 4.

    PushRosNamespace and SetRemap are the user's own tools for placing
    topics; the scenario proves they reach the MCU's topics through them.
    """
    actions = [include_bridge(port, params_file)]
    if remappings:
        actions = [SetRemap(src, dst) for src, dst in remappings] + actions
    if namespace:
        actions = [PushRosNamespace(namespace)] + actions
    if len(actions) == 1:
        return actions[0]
    return GroupAction(actions)


def launch_bridge_only(port, params_file, namespace=None, remappings=()):
    """A launch description with the bridge and nothing else.

    The scenarios spawn the firmware themselves once the bridge is up, so
    the 3 s budget of scenario 1 measures the handshake, not node startup.
    """
    return launch.LaunchDescription(
        [
            scoped_include_bridge(port, params_file, namespace, remappings),
            launch_testing_actions_ready(),
        ]
    )


def wait_for_bridge(node, service="/r2s_bridge/get_parameters", timeout=HANDSHAKE_TIMEOUT):
    """Block until the bridge answers its parameter service, proving it is up."""
    client = node.create_client(GetParameters, service)
    try:
        assert client.wait_for_service(timeout_sec=timeout), "bridge parameter service never appeared"
    finally:
        node.destroy_client(client)


def wait_for_topics(node, wanted, since, budget, what):
    """Block until every wanted topic is listed, within budget of since."""
    missing = set(wanted)
    while missing:
        names = {name for name, _ in node.get_topic_names_and_types()}
        missing = set(wanted) - names
        if not missing:
            return time.monotonic() - since
        assert time.monotonic() - since < budget, what + " missing after %.1f s: %s" % (
            budget,
            sorted(missing),
        )
        time.sleep(0.1)
