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

## The bridge refuses the HELLO for a protocol version mismatch

The log shows this line once at error level, then at debug level while the
same mismatch repeats:

```text
HELLO refused: the MCU protocol version is 3, the host protocol version is 4
```

`/diagnostics` carries the same text under `handshake refused` at level
ERROR while the refusals continue. The protocol version must match exactly:
no other version is accepted. Regenerate and reflash the firmware library
from the same checkout as the bridge, or install a matching bridge, so both
sides report the same version.

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
blocking spin. Check string/sequence bounds ([where they come from](message-bounds.md))
when the bridge reports a field error. A buffer setting in a sketch alone does not configure the library:
compile with `ros2 run r2s_gen r2s-arduino compile`, or apply the generated limits
to every translation unit in your own build.

An application that blocks for longer than the link timeout can appear unplugged.
Use nonblocking driver logic and frequent spin calls before increasing timeouts.

## No topics from an ESP32: `Serial` is not the port in use

The sketch talks on `Serial`, and a board option decides which physical
port that is. If the board was built with `USB CDC On Boot: Enabled` but
the bridge is on the `COM`/`UART` port, or the other way round, the
firmware runs on the other port and the bridge hears silence. Rebuild
with the FQBN and the `USB CDC On Boot` setting for the port in use, as
[ESP32 boards](esp32.md#two-port-boards-pick-one-port-and-build-for-it)
spells out. On the native USB build `Serial` is USB and `Serial0` is
UART0, so a sketch that must use UART0 there names `Serial0`.

## An ESP32 board is held in reset or download mode

The auto-reset circuit lets a host tool drive the board into reset or
download mode through the port's control lines. A tool that exited
while it left RTS asserted can leave the board held in that state, so
it never starts the sketch and no topics appear. Close every program
that held the port, unplug and replug the board, and press its reset
or EN button if it has one. Then start the bridge once, with nothing
else owning the port.

## The ESP32 port name changes after a reset

The port name changing after a reset is normal on the native USB
port: a reset makes the board re-enumerate, and the host may give it
the next `ttyACMn` name. A bridge still pointing at the old name then
cannot open the port. Use the stable name under `/dev/serial/by-id/`
for the launch and for `make hardware-check`, as
[ESP32 boards](esp32.md#tell-the-ports-apart-on-the-host) shows.

## A useful bug report

Include the checkout commit, `ros2 run r2s_gen r2s-gen version`, ROS distro,
board/FQBN, Arduino core version, launch command, relevant log lines, and the
smallest sketch that reproduces the behavior. State whether it happens on real
hardware or a simulator. See [Contributing](../CONTRIBUTING.md) for development
and validation commands.
