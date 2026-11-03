# Port your rosserial sketch to ros2serial

A rosserial sketch ports by changing its includes and its message
fields. The calls stay the same: `initNode`, `advertise`, `subscribe`,
`spinOnce`, plus `connected`, `now`, `setSpinTimeout`, `getHardware`
and the `log*` diagnostics. This guide covers what is on `master`:
the aliases from [#145](https://github.com/apojomovsky/ros2serial/issues/145)
and the rest of the handle surface from
[#417](https://github.com/apojomovsky/ros2serial/issues/417).
The `tf` broadcaster and services are not there yet; see
[Not yet](#not-yet) for what waits on
[#418](https://github.com/apojomovsky/ros2serial/issues/418).

What does not change: the wiring, the bridge launch, and the ROS 2
side. The sketch still opens the serial port at 115200 baud, the
bridge still republishes the topics on the ROS 2 graph, and the node
announces itself as `rosserial`, since a rosserial sketch names no
node of its own.

## 1. Install the library

Follow the README walkthrough steps 1 and 2 first: the generator, the
bundle, the core, the Arduino port glue, and the runtime layer. Then
add the compatibility headers, which live in `cpp/include` and ride
into the library beside the runtime header:

```sh
LIB=~/Arduino/libraries/ros2serial
cp -r cpp/include/ros2serial "$LIB/src/"
```

## 2. Swap the includes

Every sketch keeps the bundle sketch header first: it pulls in the
bundled types and the runtime layer, and it is the include the Arduino
builder resolves the library from. The rosserial includes gain the
`ros2serial/` prefix below it. The handle, publisher, subscriber and
clock types come from one header; each message type comes from its own
header, so a sketch includes only the types it uses:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/std_msgs/Float32.h>
#include <ros2serial/geometry_msgs/Twist.h>
```

| ros_lib | ros2serial |
| --- | --- |
| `ros.h` | `ros2serial/ros.h` |
| `std_msgs/Float32.h` | `ros2serial/std_msgs/Float32.h` |
| `geometry_msgs/Twist.h` | `ros2serial/geometry_msgs/Twist.h` |

The pattern holds for every bundled type: `std_msgs/String.h`,
`std_msgs/Header.h`, `geometry_msgs/Vector3.h`,
`geometry_msgs/Quaternion.h`, `sensor_msgs/Imu.h`,
`sensor_msgs/JointState.h`, and `builtin_interfaces/Time.h`.

## 3. Keep the sketch

A minimal rosserial sketch looks like this:

<!-- snippet: skip until #149: ros_lib original for contrast, ros_lib headers are not in this tree -->
```cpp
#include <ros.h>
#include <std_msgs/Float32.h>
#include <geometry_msgs/Twist.h>

ros::NodeHandle nh;
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);

void on_cmd_vel(const geometry_msgs::Twist &msg)
{
    (void)msg;
}
ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub("cmd_vel", &on_cmd_vel);

void setup()
{
    nh.initNode();
    nh.advertise(battery_pub);
    nh.subscribe(cmd_vel_sub);
}

void loop()
{
    battery.data = 1.0f;
    battery_pub.publish(&battery);
    nh.spinOnce();
}
```

The port keeps every line except the includes, which gain the bundle
header and the prefix:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/std_msgs/Float32.h>
#include <ros2serial/geometry_msgs/Twist.h>

ros::NodeHandle nh;
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);

void on_cmd_vel(const geometry_msgs::Twist &msg)
{
    (void)msg;
}
ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub("cmd_vel", &on_cmd_vel);

void setup()
{
    nh.initNode();
    nh.advertise(battery_pub);
    nh.subscribe(cmd_vel_sub);
}

void loop()
{
    battery.data = 1.0f;
    battery_pub.publish(&battery);
    (void)nh.spinOnce();
}
```

A `NodeHandle_<ArduinoHardware, 1, 1, 64, 64>` spelling still
compiles: the size parameters are accepted and ignored, since every
registration runs on the runtime fixed RAM table either way. A member
function callback uses the two-parameter subscriber, as in ros_lib:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/geometry_msgs/Twist.h>

ros::NodeHandle nh;

class Echo
{
  public:
    void on_twist(const geometry_msgs::Twist &msg)
    {
        (void)msg;
    }
};

Echo echo;
ros::Subscriber<geometry_msgs::Twist, Echo> echo_sub("echo_vel", &Echo::on_twist, &echo);

void setup()
{
    nh.initNode();
    nh.subscribe(echo_sub);
}
```

The subscriber keeps its own message store and dispatches into it, so
the subscriber object must outlive the spinning node: keep it at file
scope, as above.

## 4. Fix the message fields

Fixed size messages need no edit beyond the include: `Float32`,
`Twist`, `Vector3`, `Quaternion`, `Imu`, and `Time` hold the same
fields with the same widths. Three shapes need one line each.

Strings are fixed buffers of 16 characters plus the terminator, so
copy instead of assigning a pointer:

```cpp
#include <ros2serial.h>
#include <stdio.h>
#include <ros2serial/std_msgs/String.h>

void fill_greeting(std_msgs::String &msg, const char *text)
{
    (void)snprintf(msg.data, sizeof msg.data, "%s", text);
}
```

Longer text truncates at 16 characters and stays terminated. Sending
a string without a terminator inside the buffer fails the publish
with the bounds error instead of emitting bad bytes.

Bounded arrays carry their length beside the storage, so set the size
next to the elements. `JointState` holds 6 names and 6 entries per
column:

```cpp
#include <ros2serial.h>
#include <stdio.h>
#include <ros2serial/sensor_msgs/JointState.h>

void fill_joints(sensor_msgs::JointState &msg)
{
    msg.position.size = 2u;
    msg.position.data[0] = 1.5;
    msg.position.data[1] = -0.5;
    msg.name.size = 2u;
    (void)snprintf(msg.name.data[0], sizeof msg.name.data[0], "%s", "left");
    (void)snprintf(msg.name.data[1], sizeof msg.name.data[1], "%s", "right");
}
```

Publishing with a size past the bound fails instead of sending, as
with strings. Fixed width arrays such as the `Imu` covariances fill
element by element with no size to set.

`Header` combines both: `frame_id` is a 16 character buffer, and
`stamp` keeps its `sec` and `nanosec` fields:

```cpp
#include <ros2serial.h>
#include <stdio.h>
#include <ros2serial/std_msgs/Header.h>

void fill_frame(std_msgs::Header &msg)
{
    msg.stamp.sec = 12;
    msg.stamp.nanosec = 500000000u;
    (void)snprintf(msg.frame_id, sizeof msg.frame_id, "%s", "base_link");
}
```

## 5. Read the clock, log, and guard the spin

`now` returns a `ros::Time` with `sec` and `nsec`, and the arithmetic
copies ros_lib (`toSec`, `fromSec`, `toNsec`, `fromNSec`, `+=`, `-=`,
and the `Time` minus `Time` span). On an Arduino build the clock has
no host correction, so `now` counts milliseconds since boot, as an
unsynced ros_lib clock does:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>

ros::NodeHandle nh;

void loop()
{
    ros::Time stamp = nh.now();
    ros::Duration timeout(1, 0);
    stamp += timeout;
    (void)stamp.toSec();
}
```

The five log calls send one frame each on the log channel, and
compile out where the channel is off, so one sketch builds for every
tier. On Arduino the `F()` overload keeps flash text in flash:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>

ros::NodeHandle nh;

void setup()
{
    nh.initNode();
    nh.loginfo("boot");
}
```

`connected` reports the handshake, `setSpinTimeout` bounds one
`spinOnce` call in milliseconds (zero disables it, as in ros_lib),
and `getHardware` hands back the port handle sketches expect:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/std_msgs/Float32.h>

ros::NodeHandle nh;
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);

void setup()
{
    nh.initNode();
    nh.setSpinTimeout(100u);
    (void)nh.getHardware();
    nh.advertise(battery_pub);
}

void loop()
{
    int rc = nh.spinOnce();
    if (rc == ros::SPIN_TIMEOUT) {
        return;
    }
    if (nh.connected()) {
        battery.data = 1.0f;
        battery_pub.publish(&battery);
    }
}
```

Two refusals to know: `advertise` and `subscribe` return false when
the table is full or the first spin already closed it, announcing
nothing. `publish` returns 0 when the frame went out and -1 when
nothing was sent: an unannounced topic, a silent link, a refused
topic, or a message that does not fit.

## 6. Compile and run

Compile the ported sketch the way the README compiles the example:
the same library, the same limit flags read from the bundle, the
board of your choice:

```sh
EXTRAS=$(python3 - "$LIB/src/r2s_bundled.h" <<'EOF'
import sys

from r2s_gen import layouts
from r2s_gen.msgs import MessageError

try:
    defines = layouts.build_defines(open(sys.argv[1]).read(), "src/r2s_bundled.h")
except (MessageError, OSError) as error:
    print(f"r2s-gen: error: {error}", file=sys.stderr)
    raise SystemExit(1)
print(" ".join("-D" + define for define in defines))
EOF
)
arduino-cli compile --fqbn arduino:avr:uno \
  --build-property "compiler.c.extra_flags=$EXTRAS" \
  --build-property "compiler.cpp.extra_flags=$EXTRAS" \
  --library "$LIB" path/to/your_sketch
```

Run it exactly as the README runs the example: start the firmware on
simavr (or flash it), launch the bridge on the port, and watch the
topic. The bridge default baud is 115200, matching `initNode`.

## Not yet

Two ros_lib pieces have no counterpart yet and wait on
[#418](https://github.com/apojomovsky/ros2serial/issues/418): the
`tf::TransformBroadcaster` (`init` and `sendTransform`) and services
(`advertiseService` and `serviceClient`, which will fail to compile
with a message naming the services ticket until services land).
Leave that code out of the port for now. The same ticket tracks the
remaining proof: every end to end scenario passing on the
compatibility path.
