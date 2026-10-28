"""Launch the r2s bridge on one serial port.

The port and the parameter file stay launch arguments, so tests and users
drive them without editing this file: port names the serial device, and
params_file selects the parameter set, the example one by default. Namespace
and remappings use the standard scoping actions around the inclusion.
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


def generate_launch_description():
    port = LaunchConfiguration("port")
    params_file = LaunchConfiguration("params_file")
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
            OpaqueFunction(function=check_params_file),
            Node(
                package="r2s_bridge",
                executable="bridge",
                name="r2s_bridge",
                parameters=[params_file, {"port": port}],
                output="screen",
            ),
        ]
    )
