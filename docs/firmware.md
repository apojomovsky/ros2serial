# Add ROS topics to your firmware

Start from the button/LED sketch installed under
`firmware/ros2serial/examples/button_led/` by the [quickstart](../README.md).
Copy it into your firmware project before editing. Keep the directory and `.ino`
file names equal, as Arduino CLI requires.

## Declare your interface

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

Use Arduino CLI to upload as shown in the README. The helper passes the library's
buffer and feature definitions to every compilation unit. Defining those values
only at the top of the sketch does not configure Arduino's separately compiled
library sources. This helper handles bundled-type topic sketches; generated
custom interfaces use the [custom interface workflow](custom-interfaces.md).

## Message storage and limits

Include the generated embedded header, such as `<sensor_msgs/msg/range.hpp>`.
These types use fixed storage; they are not the host's heap-allocating C++ message
classes. Strings are bounded NUL-terminated arrays. Variable-length sequences
carry storage plus a length field. Inspect the generated header before assigning
these fields. Out-of-bounds payloads fail rather than silently truncating.

The default bundle supports common scalar, geometry, sensor and TF types.
It bounds strings to 16 characters by default, `JointState` to six joints and
`TFMessage` to one transform. Use [generation](custom-interfaces.md) to change
bounds or add a message type. Large messages and high publish rates can exhaust
RAM or serial bandwidth even when the sketch compiles.

AVR `double` is 32 bits: `float64` fields are sent with that precision and widened
on the host. The time channel can synchronize timestamps with the host. `node.now()` is
milliseconds since boot, not a ROS `Time` object. Use the time API for stamped messages
as described in the [design's user API](00-design.md#user-api).

Do not print debugging text to the serial stream used by ros2serial. Use its
logging API (forwarded through the bridge to `/rosout`) or a separate debug port.
For compatibility logging, time, and existing ROS 1 sketches, see the
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
| `tf2_msgs/msg/TFMessage` |
