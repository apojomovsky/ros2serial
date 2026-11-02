"""Launch the r2s bridge on one serial port.

The port and the parameter file stay launch arguments, so tests and users
drive them without editing this file: port names the serial device, and
params_file selects the parameter set, the example one by default. log_level
sets the node's ROS log level, and mcu_log_level sets the `<node>.mcu` child
logger alone, debug when a test asserts on the MCU's debug lines without
drowning the node in its own debug output. Namespace and remappings use the
standard scoping actions around the inclusion.
"""

import os

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def check_params_file(context):
    """Fail fast on a missing params file, which launch_ros only warns about
    before starting the node on its built-in defaults."""
    path = LaunchConfiguration("params_file").perform(context)
    if not os.path.isfile(path):
        raise RuntimeError("params_file is not a file: " + path)


def add_bridge(context):
    """Start the bridge with the launch's log levels.

    The mcu override names the node's default child logger; a remapped node
    keeps the global level. An empty mcu_log_level sets no override.
    """
    log_level = LaunchConfiguration("log_level").perform(context)
    mcu_log_level = LaunchConfiguration("mcu_log_level").perform(context)
    arguments = ["--ros-args", "--log-level", log_level]
    if mcu_log_level:
        arguments += ["--log-level", "r2s_bridge.mcu:=" + mcu_log_level]
    return [
        Node(
            package="r2s_bridge",
            executable="bridge",
            name="r2s_bridge",
            parameters=[LaunchConfiguration("params_file"), {"port": LaunchConfiguration("port")}],
            arguments=arguments,
            output="screen",
        )
    ]


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "port",
                default_value="/dev/ttyACM0",
                description="Serial device the MCU is attached to.",
            ),
            DeclareLaunchArgument(
                "params_file",
                default_value=[FindPackageShare("r2s_bridge"), "/config/r2s_bridge.params.yaml"],
                description="Parameter file holding every r2s_bridge parameter.",
            ),
            DeclareLaunchArgument(
                "log_level",
                default_value="info",
                description="ROS log level for the bridge node.",
            ),
            DeclareLaunchArgument(
                "mcu_log_level",
                default_value="",
                description="ROS log level for the bridge's mcu child logger alone.",
            ),
            OpaqueFunction(function=check_params_file),
            OpaqueFunction(function=add_bridge),
        ]
    )
