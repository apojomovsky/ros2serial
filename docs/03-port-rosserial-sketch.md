# Port your rosserial sketch to ros2serial

A rosserial sketch ports by changing its includes and its message
fields. The calls stay the same: `initNode`, `advertise`, `subscribe`,
`advertiseService`, `serviceClient`, `spinOnce`, plus `connected`,
`now`, `setSpinTimeout`, `getHardware` and the `log*` diagnostics.
Parameters read through `getParam`. This guide covers what is on
`master`: the aliases from
[#145](https://github.com/apojomovsky/ros2serial/issues/145)
and the rest of the handle surface from
[#417](https://github.com/apojomovsky/ros2serial/issues/417),
plus the `tf` broadcaster from
[#418](https://github.com/apojomovsky/ros2serial/issues/418)
(see [Broadcast tf](#broadcast-tf)), the services and parameters from
[#447](https://github.com/apojomovsky/ros2serial/issues/447)
(see [Services](#services) and [Parameters](#parameters)).
Actions have no rosserial original, so the native server stands alone;
see [Actions](#actions).

What does not change: the wiring, the bridge launch, and the ROS 2
side. The sketch still opens the serial port at 115200 baud, the
bridge still republishes the topics on the ROS 2 graph, and the node
announces itself as `rosserial`, since a rosserial sketch names no
node of its own.

## 1. Install the library

Build and source the ROS workspace as shown in [Installation](installation.md),
then prepare the complete library, including the compatibility headers:

```sh
ros2 run r2s_gen r2s-arduino library --out firmware/ros2serial
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

Compile a topic-only ported sketch with the installed helper:

```sh
ros2 run r2s_gen r2s-arduino compile \
  --library firmware/ros2serial --fqbn arduino:avr:uno \
  --out firmware/build path/to/your_sketch
```

Upload with Arduino CLI and launch the bridge as in the [README](../README.md).
The bridge default baud is 115200, matching `initNode`.
Services and parameters require their feature flags in every translation unit;
see [Services and parameters](services-and-parameters.md).

## Broadcast tf

The `tf::TransformBroadcaster` keeps its ros_lib shape (`init` and
`sendTransform`) over the compatibility publisher, announcing `/tf`
carrying `TFMessage`:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/geometry_msgs/TransformStamped.h>
#include <ros2serial/tf/transform_broadcaster.h>

ros::NodeHandle nh;
tf::TransformBroadcaster broadcaster;
geometry_msgs::TransformStamped transform;

void setup()
{
    nh.initNode();
    broadcaster.init(nh);
}

void loop()
{
    broadcaster.sendTransform(transform);
    (void)nh.spinOnce();
}
```

## Services

Services keep their ros_lib shape: `advertiseService` serves one
service through a callback, `serviceClient` names one to call, and
`call` sends a request and spins until its answer. The request and
response types come from the service header, which the compatibility
header renames the ros_lib way:

```cpp
#include <ros2serial.h>
#include <ros2serial/ros.h>
#include <ros2serial/std_srvs/Trigger.h>

ros::NodeHandle nh;

void on_reset(const std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    (void)req;
    res.success = true;
}

ros::ServiceServer<std_srvs::Trigger::Request, std_srvs::Trigger::Response> reset_srv("reset",
    &on_reset);
ros::ServiceClient<std_srvs::Trigger::Request, std_srvs::Trigger::Response> trigger_cli(
    "trigger");

void setup()
{
    nh.initNode();
    nh.advertiseService(reset_srv);
    nh.serviceClient(trigger_cli);
}

void loop()
{
    std_srvs::Trigger::Request req;
    std_srvs::Trigger::Response res;

    if (trigger_cli.call(req, res) && res.success) {
        nh.loginfo("trigger answered");
    }
    (void)nh.spinOnce();
}
```

Two differences to know. `call` returns whether the answer arrived:
`true` with the host's ok answer, `false` on refusal, timeout or
disconnect. Ignoring the result still compiles, so a ported sketch
builds either way. And the announce closes with the first spin, like
topics: `advertiseService` and `serviceClient` past it announce
nothing and return false. Without the services channel the calls
compile and report the refusal, so one sketch builds for every tier.
The `examples/arduino_service` sketch shows both directions on the
native API: it serves `reset` and calls `trigger`, with the same
Trigger pair the ported sketch above uses.

## Parameters

Parameters differ from rosserial: ros2serial parameters are declared
by the MCU and exposed on the bridge node, while rosserial's
`getParam` reads the host parameter server. The ported sketch declares
its table before the first spin, then reads the live values through
`getParam`: a host set lands at the next spin, and the sketch reads
what the last spin applied.

```cpp
#include <ros2serial.h>
#include <r2s/r2s_params.h>
#include <ros2serial/ros.h>

static char label_live[17];
static r2s_param_t params[2];

ros::NodeHandle nh;

void setup()
{
    params[0].name = "led";
    params[0].type = R2S_PARAM_BOOL;
    params[0].initial.b = 1u;
    params[1].name = "label";
    params[1].type = R2S_PARAM_STRING;
    params[1].bound = 16u;
    params[1].initial.s = "hello";
    params[1].value.s = label_live;
#ifdef R2S_WITH_PARAMS
    (void)r2s_params_configure(params, 2);
#endif
    nh.initNode();
}

void loop()
{
    bool led = false;
    char text[17];
    char *slot = text;

    if (nh.getParam("led", &led) && led) {
        nh.loginfo("led is on");
    }
    if (nh.getParam("label", &slot)) {
        nh.loginfo(slot);
    }
    (void)nh.spinOnce();
}
```

The guard keeps one sketch building with the channel off: then the
configure call compiles out and `getParam` reads nothing. The four
overloads (`bool*`, `int*`, `float*`, `char**`) map onto the declared
types (`R2S_PARAM_BOOL`, `R2S_PARAM_I64`, `R2S_PARAM_F64`,
`R2S_PARAM_STRING`): an unknown name, a type mismatch, a length other
than 1, or an integer outside int range reads nothing and returns
false. The timeout is accepted and ignored, since nothing crosses the
wire. Anything else, an array or another width, fails to compile
naming the rule.

## Actions

rosserial never had actions, so there is nothing to port: the native
`r2s::action::Server` serves goals straight from the sketch, as
`examples/arduino_action` shows with one Fibonacci-like goal. The bridge
presents that goal as a ROS 2 action server
([#584](https://github.com/apojomovsky/ros2serial/pull/584)). The sketch
stands in Float32 payloads under a placeholder type, so drive a ROS 2 client
against firmware announcing an installed action type instead; see
[Services and parameters](services-and-parameters.md).
