# Examples

Start with the [button/LED walkthrough](../README.md). To prepare the Arduino
examples in a writable project directory:

```sh
ros2 run r2s_gen r2s-arduino library --out firmware/ros2serial
```

The installed examples use short Arduino sketch names such as `button_led`;
the repository names them `arduino_button_led`. Each installed Arduino sketch directory
contains an `.ino` of the same name. Copy a sketch out of the library before adapting it.

| Sketch | MCU publishes | MCU subscribes | Purpose |
| --- | --- | --- | --- |
| `button_led` | `button` (Bool) | `led` (Bool) | Bidirectional first connection |
| `hello` | `chatter` (String) | | Bounded text |
| `blink` | | `led` (Bool) | Drive an output |
| `button` | `button` (Bool) | | Debounced input changes |
| `servo` | | `servo` (Float32) | Command an actuator |
| `range` | `range` (Range) | | Fill a sensor message; reading is a placeholder |
| `minimal` | `battery` (Float32) | `cmd_vel` (Twist) | Typical robot interface |
| `service` | | serves `reset`, calls `trigger` (Trigger) | Serve a service; the MCU also calls `trigger` |
| `static`, `xmacro`, `xmacro_c` | See sketch | See sketch | Static topic declarations |
| `action` | Action feedback/result | Action goals | MCU action API over placeholder types; no ROS client drives it |

Compile topic examples with the library's required settings, then upload and
launch the bridge as shown in the README:

```sh
ros2 run r2s_gen r2s-arduino compile \
  --library firmware/ros2serial --fqbn arduino:avr:uno \
  --out firmware/build firmware/ros2serial/examples/hello
```

The action example needs additional feature flags and a smaller frame limit;
it is not built by this topic-only command. See
[Services and parameters](../docs/services-and-parameters.md).

The examples show ROS integration, not sensor wiring or device drivers. Replace
placeholder readings and callbacks with your own drivers, preserving frequent
spin calls. `button` publishes changes only; `button_led` repeats state so late
subscribers see the current value.

For other build systems, `cmake_minimal` demonstrates generated C firmware on
AVR, `pio_hello` uses PlatformIO, and `ch32v_minimal` demonstrates the CH32V port.
See [Custom interfaces](../docs/custom-interfaces.md) for generation and
[Without hardware](../docs/without-hardware.md) for host simulation.

`custom_interfaces` is the custom-type walkthrough: a ROS interface package
with one message, one service and one action, and a firmware that uses all
three through the generated static API. It builds with CMake for the
ATmega328P, and an end-to-end scenario runs it against the bridge. Start there
when your robot needs its own types instead of the bundled ones.
