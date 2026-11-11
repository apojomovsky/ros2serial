# Launch ros2serial with your robot

After the [first connection](../README.md), place the bridge in your robot's
bringup package. Keep firmware topic names relative and configure placement on
the host, so one sketch can serve different robots without reflashing.

## Launch and run

```sh
ros2 launch r2s_bridge r2s_bridge.launch.py \
  port:=/dev/ttyACM0 namespace:=panel remappings:='button:=pressed,led:=indicator'
```

This exposes `/panel/pressed` and `/panel/indicator`. The node is
`/panel/r2s_bridge`. To run the executable directly:

```sh
ros2 run r2s_bridge bridge --ros-args \
  -p port:=/dev/ttyACM0 -p baud:=115200 -r __ns:=/panel -r button:=pressed
```

Use `/dev/serial/by-id/...` when your USB adapter provides a stable identity.
Find it with `ls -l /dev/serial/by-id/`; use that path for upload and launch.
Identical adapters without unique serial numbers may need a local udev rule.

## Include from a launch file

Install this as a launch file in your robot's bringup package, and declare
`r2s_bridge` as an `exec_depend` in its `package.xml`:

```python
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    return LaunchDescription(
        [
            DeclareLaunchArgument('panel_port', default_value='/dev/ttyACM0'),
            IncludeLaunchDescription(
                PythonLaunchDescriptionSource(
                    PathJoinSubstitution(
                        [
                            FindPackageShare('r2s_bridge'),
                            'launch',
                            'r2s_bridge.launch.py',
                        ]
                    )
                ),
                launch_arguments={
                    'port': LaunchConfiguration('panel_port'),
                    'namespace': 'panel',
                    'remappings': 'button:=pressed,led:=indicator',
                }.items(),
            ),
        ]
    )
```

For two boards, include the bridge launch file twice with distinct ports and
namespaces. Each inclusion starts one bridge process. A component-based setup
can instead load `r2s_bridge::Bridge` instances into a component container; each
instance still owns one serial port. Never open the same port twice.

## Configuration

The launch file accepts `port`, `params_file`, `namespace`, `remappings`,
`log_level`, and `mcu_log_level`. Inspect them with:

```sh
ros2 launch r2s_bridge r2s_bridge.launch.py --show-args
```

`baud` is a node parameter, not a launch argument. Put it in a parameter file
or pass it to `ros2 run` as above. Match the firmware's `node.begin()` baud.
For a namespaced node, use a wildcard or the fully qualified node name in YAML:

```yaml
/**:
  ros__parameters:
    baud: 115200
    boot_wait_ms: 2500
    rate_limit.default_hz: 20.0
    rate_limit.overrides: ["led=10"]
    tx_budget_pct: 50
    qos_depth: 10
```

Save this as `panel.yaml`, then launch:

```sh
ros2 launch r2s_bridge r2s_bridge.launch.py \
  port:=/dev/ttyACM0 namespace:=panel params_file:=/absolute/path/to/panel.yaml
```

Rate limits apply to host-to-MCU traffic and name firmware topics before
remapping. They do not throttle the MCU's publishers; set those rates in the
sketch. Omitted values use the node defaults. Startup configuration is not a
promise that a parameter can be changed while the link runs.

## Observe the link

```sh
ros2 node info /panel/r2s_bridge
ros2 topic info /panel/pressed --verbose
ros2 topic echo /diagnostics diagnostic_msgs/msg/DiagnosticArray --once
```

Diagnostics report link state, topic acceptance, drops, and time synchronization.
MCU logs appear under the bridge's `.mcu` logger and in `/rosout`. Enable bridge
logging with `log_level:=debug` when diagnosing connection problems.
Topics are best effort over serial even when a host DDS endpoint requests
reliability; do not infer end-to-end delivery guarantees from DDS QoS alone.
