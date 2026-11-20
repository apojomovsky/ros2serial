# Add ROS topics to your firmware

Start from the button/LED sketch installed under
`firmware/ros2serial/examples/button_led/` by the [quickstart](../README.md).
Copy it into your firmware project before editing. Keep the directory and `.ino`
file names equal, as Arduino CLI requires.

## Declare your interface

This guide uses the runtime objects, which is the right starting point on any
board. When the sketch runs short of RAM you can move the same declarations into
an X-macro list without touching the rest of the sketch:
[Choosing how to declare your topics](declaring-topics.md) compares the two and
says what each costs. Firmware compiled as C, or built without the Arduino
library, starts from [Building in C or without the Arduino
library](c-and-bare-metal.md).

The runtime API follows ROS 2 message naming. Declarations live at file scope:

```cpp
#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

r2s::Node node("panel");
r2s::Publisher<std_msgs::msg::Bool> button_pub(node, "button");

void on_led(const std_msgs::msg::Bool &msg)
{
    // Replace this body with your actuator driver.
    (void)msg;
}
r2s::Subscription<std_msgs::msg::Bool> led_sub(node, "led", on_led);
```

The publisher sends MCU data to ROS; the subscription receives ROS commands.
Use relative topic names so the robot launch file can namespace and remap them.
Declare all topics before the first spin. `panel` identifies the firmware;
`r2s_bridge` is the host ROS node, not a second ROS node named `panel`.

In Arduino `setup()`, call `node.begin(Serial, 115200)`. In `loop()`, call
`node.spin_some()` frequently, read your driver, fill the message, and call
`button_pub.publish(msg)`. Callbacks run during spin: keep them short and avoid
blocking sensor reads, long delays, or waiting for another ROS message.

The checked-in button/LED example debounces without delays and republishes
state at 10 Hz. Periodic state is useful after reconnects and for subscribers
that start later. An event published once before a connection exists is lost.
Check `node.connected()` before publishing and inspect the publish result:
`R2S_OK` means accepted, `R2S_ERR_TX_BUSY` means retry or drop according to your
application, and `R2S_ERR_NOT_CONNECTED` means the link is not ready.
Do not retry in a tight loop that prevents spin from servicing the link.

## Compile your sketch

With the workspace sourced, from its root:

```sh
ros2 run r2s_gen r2s-arduino compile \
  --library firmware/ros2serial --fqbn arduino:avr:uno \
  --out firmware/build path/to/my_sketch
```

Use Arduino CLI to upload as shown in the README. The staged library carries
its own `src/r2s_config.h`, so a plain `arduino-cli compile --library` needs no
extra property; the helper passes the bundle's flags as well, which override
the file with the values it read from `r2s_bundled.h`. Defining those values
only at the top of the sketch does not configure Arduino's separately compiled
library sources. This helper handles bundled-type topic sketches; generated
custom interfaces use the [custom interface workflow](custom-interfaces.md).

### The frame limit is a build input

`R2S_MAX_FRAME` sizes the receive buffer in the core's own source files and the
core announces that same number in its handshake, so the buffers and the
announced value must be one number. A staged library carries it in
`src/r2s_config.h`, which the core's own sources include, so every route that
compiles a library is configured by the library itself. A build that carries
neither the file nor the flag still fails with `R2S_MAX_FRAME is not set: pass
-DR2S_MAX_FRAME=<n> to every translation unit`, rather than compiling one limit
and announcing another.

Where the value comes from on each route:

| Route | Where the value comes from | Overriding it |
| --- | --- | --- |
| Arduino IDE (`Sketch > Include Library > Add .ZIP Library`) | The library's `src/r2s_config.h`: nothing to configure | A `platform.local.txt` beside the AVR core's `platform.txt` with `compiler.c.extra_flags=-DR2S_MAX_FRAME=<n>` and the `compiler.cpp.extra_flags` line |
| A type library from `r2s-gen types`, zipped or not | The same `src/r2s_config.h`, which that library emits too, or the archive `--zip` writes | A flag on your own compile line |
| `ros2-arduino compile` (`r2s-arduino`) | The same file, plus `-DR2S_MAX_FRAME=<n>` read from `r2s_bundled.h`, which it still passes explicitly | The flag the helper passes, which wins over the file |
| Arduino CLI by hand against a staged library | The library's `src/r2s_config.h` | `--build-property "compiler.c.extra_flags=-DR2S_MAX_FRAME=<n>"`, and the same for `compiler.cpp.extra_flags` |
| CMake (`--layout cmake`) | The generated `CMakeLists.txt` reads the value from `r2s_generated.h` and passes it as a target definition | The generator's `limits.max_frame` |
| PlatformIO (`--layout platformio`) | The generated `library.json` carries it in `build.flags` | The generator's `limits.max_frame` |
| Bare metal, any other build | Your own compile line | `-DR2S_MAX_FRAME=<n>` |

Whichever value reaches the build first wins: every limit in `r2s_config.h`
keeps its `#ifndef`, so a flag an application passes overrides the shipped
one. `docs/services-and-parameters.md` shows the flag strings for the service
and action examples, which lower the limit to fit a smaller part.

## Message storage and limits

Include the generated embedded header, such as `<sensor_msgs/msg/range.hpp>`.
These types use fixed storage; they are not the host's heap-allocating C++ message
classes. Strings are bounded NUL-terminated arrays. Variable-length sequences
carry storage plus a length field. Inspect the generated header before assigning
these fields. Out-of-bounds payloads fail rather than silently truncating.

The default bundle supports common scalar, geometry, sensor and TF types. Its
limits are chosen, not inherited from the definitions, so check them before
relying on a capacity you did not pick: [message bounds](message-bounds.md) has
the table and the command that prints it. Large messages and high publish rates
can exhaust RAM or serial bandwidth even when the sketch compiles.

AVR `double` is 32 bits: `float64` fields are sent with that precision and widened
on the host. The time channel synchronizes timestamps with the host once the firmware
is built with `R2S_WITH_TIME`, which the bundled helper leaves out. `node.now()` is
milliseconds since boot, not a ROS `Time` object. The [design's Time
section](00-design.md#time) describes the protocol.

Do not print debugging text to the serial stream used by ros2serial. Use its
logging API, which the bridge forwards to `/rosout` once the firmware defines
`R2S_WITH_LOG`, or a separate debug port.
To port an existing ROS 1 rosserial sketch, logging and time included, see the
[migration guide](03-port-rosserial-sketch.md).

## Bundled types

| Type |
| --- |
| `builtin_interfaces/msg/Time` |
| `geometry_msgs/msg/Quaternion` |
| `geometry_msgs/msg/Transform` |
| `geometry_msgs/msg/TransformStamped` |
| `geometry_msgs/msg/Twist` |
| `geometry_msgs/msg/Vector3` |
| `sensor_msgs/msg/Imu` |
| `sensor_msgs/msg/JointState` |
| `sensor_msgs/msg/Range` |
| `std_msgs/msg/Bool` |
| `std_msgs/msg/Float32` |
| `std_msgs/msg/Header` |
| `std_msgs/msg/String` |
| `std_srvs/srv/Trigger_Request` |
| `std_srvs/srv/Trigger_Response` |
| `tf2_msgs/action/LookupTransform_Feedback` |
| `tf2_msgs/action/LookupTransform_Goal` |
| `tf2_msgs/action/LookupTransform_Result` |
| `tf2_msgs/msg/TFMessage` |
