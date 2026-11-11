# Troubleshooting

Keep the bridge in the foreground while diagnosing a connection. Start with the
port and handshake, then check the ROS graph, then inspect payloads.

## The executable or package is missing

Source the ROS underlay and your workspace's `install/setup.bash` in every
terminal. Check `ros2 pkg executables r2s_gen` and `ros2 pkg prefix r2s_bridge`.
If the workspace was built before the tools were added, rebuild it. For the
nested `r2s_rosidl` package, use the explicit discovery command in
[Custom interfaces](custom-interfaces.md#generate-as-part-of-colcon-build).

## Upload or launch cannot open the port

Run `arduino-cli board list` and check the selected path. On Ubuntu, serial
access commonly requires membership in `dialout`:

```sh
ls -l /dev/ttyACM0
id -nG
sudo usermod -aG dialout "$USER"
```

Log out and back in after changing groups. Use the group actually owning your
serial device if your distribution differs. Do not run the bridge as root or
make the port world-writable. Close serial monitors and stop the bridge before
uploading. Only one process should own the serial stream.

## The board opens but no topics appear

An Uno resets when opened; the default boot wait is 2500 ms. Check that the
firmware was flashed, the sketch calls `spin_some()` regularly, and both sides
use the same baud (115200 in the example). Debug text on that serial port
corrupts protocol frames.

```sh
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0 log_level:=debug
```

Check the log for rejected types, protocol mismatch, or failure to register the
firmware's topics. Regenerate and flash firmware from the same checkout as the
host. A type-hash mismatch means the message definitions differ; use the same
ROS distro and custom interface version on both sides.

## Topics exist but data does not arrive

```sh
ros2 topic list
ros2 topic info /button --verbose
ros2 topic echo /button std_msgs/msg/Bool
```

Use the names after remapping and namespacing. Match `ROS_DOMAIN_ID` between
terminals. Inspect endpoint QoS if discovery succeeds without matching. Confirm
the firmware is publishing after connection; a one-shot event sent before the
subscriber existed is not retained. The button/LED example repeats state.

For actuator commands, confirm the topic direction and that its callback runs
during spin.

## Drops, bounds errors, or disconnects

Inspect `/diagnostics` and the bridge log. Reduce publication rate or message
size when serial bandwidth is saturated. Handle `R2S_ERR_TX_BUSY` without
blocking spin. Check string/sequence bounds when the bridge reports a field
error. A buffer setting in a sketch alone does not configure the library:
compile with `ros2 run r2s_gen r2s-arduino compile`, or apply the generated limits
to every translation unit in your own build.

An application that blocks for longer than the link timeout can appear unplugged.
Use nonblocking driver logic and frequent spin calls before increasing timeouts.

## A useful bug report

Include the checkout commit, `ros2 run r2s_gen r2s-gen version`, ROS distro,
board/FQBN, Arduino core version, launch command, relevant log lines, and the
smallest sketch that reproduces the behavior. State whether it happens on real
hardware or a simulator. See [Contributing](../CONTRIBUTING.md) for development
and validation commands.
