# Port a rosserial sketch to the native C++ API

ros2serial ships one C++ API: `r2s::Node`, `r2s::Publisher<T>`,
`r2s::Subscription<T>`, `r2s::Service<T>`, `r2s::Client<T>` and
`r2s::action::Server<T>`, named after `rclcpp`. The rosserial-compatible layer
(`#include <ros2serial/ros.h>`, `ros::NodeHandle`, `advertise`, `subscribe`,
`spinOnce`) is gone, and with it the rosserial-style message headers such as
`ros2serial/std_msgs/Float32.h`. This page is the whole of the port: the table
below maps every include and call the layer supported, the rules after it cover
the changes that are not a rename, and the last section shows one sketch before
and after.

`ros2serial.h` and the X-macro rows, and the C API, are unchanged. The bridge,
the baud rate and the ROS 2 side of the system are unchanged too: the port is
firmware work alone.

## 1. The mapping table

Every row is one thing the removed layer accepted. `No equivalent` means the
native API has nothing with that job, and the third column says what to write
instead.

### Includes

| Removed | Native | Notes |
| --- | --- | --- |
| `#include <ros2serial/ros.h>` | `#include <ros2serial.h>` | The bundle header was already required; it now carries the runtime API alone. |
| `ros2serial/std_msgs/Float32.h` | `<std_msgs/msg/float32.hpp>` | |
| `ros2serial/std_msgs/String.h` | `<std_msgs/msg/string.hpp>` | |
| `ros2serial/std_msgs/Header.h` | `<std_msgs/msg/header.hpp>` | |
| `ros2serial/builtin_interfaces/Time.h` | `<builtin_interfaces/msg/time.hpp>` | |
| `ros2serial/geometry_msgs/Twist.h` | `<geometry_msgs/msg/twist.hpp>` | |
| `ros2serial/geometry_msgs/Vector3.h` | `<geometry_msgs/msg/vector3.hpp>` | |
| `ros2serial/geometry_msgs/Quaternion.h` | `<geometry_msgs/msg/quaternion.hpp>` | |
| `ros2serial/geometry_msgs/Transform.h` | `<geometry_msgs/msg/transform.hpp>` | |
| `ros2serial/geometry_msgs/TransformStamped.h` | `<geometry_msgs/msg/transform_stamped.hpp>` | |
| `ros2serial/sensor_msgs/Imu.h` | `<sensor_msgs/msg/imu.hpp>` | |
| `ros2serial/sensor_msgs/JointState.h` | `<sensor_msgs/msg/joint_state.hpp>` | |
| `ros2serial/std_srvs/Trigger.h` | `<std_srvs/srv/trigger.hpp>` | The service type, with its nested `Request` and `Response`. |
| `ros2serial/tf/tfMessage.h` | `<tf2_msgs/msg/tf_message.hpp>` | |
| `ros2serial/tf/transform_broadcaster.h` | no header | The broadcaster is an ordinary publisher; see [tf](#tf-broadcasting). |

Every type moves under its ROS 2 name: `std_msgs::Float32` becomes
`std_msgs::msg::Float32`, `sensor_msgs::JointState` becomes
`sensor_msgs::msg::JointState`, and `std_srvs::Trigger` becomes
`std_srvs::srv::Trigger`.

### The node handle and its calls

| Removed | Native | Notes |
| --- | --- | --- |
| `ros::NodeHandle nh;` | `r2s::Node node("name");` | The native node takes a name, which the bridge shows. |
| `ros::NodeHandle_<Hardware, sub, pub, in, out>` | `r2s::Node node("name");` | The size parameters were accepted and ignored; the runtime table is fixed either way. |
| `nh.initNode();` | `node.begin(Serial, 115200);` | On a board this starts the stream and the session; on a host port the port opens out of band. |
| `nh.advertise(pub);` | `r2s::Publisher<T> pub(node, "topic");` | The registration is the constructor. |
| `pub.publish(&msg);` | `pub.publish(msg);` | The native call takes the message by reference. |
| `pub.publish(...) == 0` / `== -1` | `pub.publish(msg) == R2S_OK` | The native call returns `r2s_status_t`; see [the publish result](#the-publish-result). |
| `nh.subscribe(sub);` | `r2s::Subscription<T> sub(node, "topic", on_msg);` | The callback is a free function or a capture-less lambda. |
| `ros::Subscriber<T, ObjT>` with a member function | `r2s::Subscription<T>` whose callback calls the member | A lambda must be capture-less to convert to the handler. |
| `sub.msg` | no equivalent | The native subscription keeps its storage private; write to your own message and publish it. |
| `pub.getEndpointType()`, `sub.getEndpointType()` | no equivalent | The endpoint value changed nothing on the wire; drop the call. |
| `nh.spinOnce();` | `node.spin_some();` | Call it as often as the old loop did. |
| `spinOnce()` returning `ros::SPIN_TIMEOUT` | no equivalent | `spin_some()` has no bound to exceed. |
| `ros::SPIN_OK`, `SPIN_ERR`, `SPIN_TX_STOP_REQUESTED`, `SPIN_TIME_RECV` | no equivalent | Only `SPIN_OK` was ever returned; compare against nothing at all. |
| `nh.connected();` | `node.connected();` | |
| `nh.setSpinTimeout(ms);` | no equivalent | Drop the call; the loop controls how long one pass takes. |
| `nh.getHardware();` | no equivalent | There is no hardware object: the port is the `Serial` stream your sketch already names. |
| `nh.now();` | `r2s_now();` | With `R2S_WITH_TIME`, the same shared clock as `builtin_interfaces::msg::Time`. Without it, `node.now()` gives milliseconds since boot. |
| `ros::Time`, `ros::Duration` | `builtin_interfaces::msg::Time`, `builtin_interfaces::msg::Duration` | Plain structs with no arithmetic; see [time arithmetic](#time-and-duration-arithmetic). |

### Services

| Removed | Native | Notes |
| --- | --- | --- |
| `ros::ServiceServer<Req, Res> srv("name", &cb);` | `r2s::Service<std_srvs::srv::Trigger> srv(node, "name", cb);` | The service type names the request and response inside it. |
| `ros::ServiceServer<Req, Res, ObjT>` with a member function | `r2s::Service<Svc>` whose callback calls the member | The handler is a free function or a capture-less lambda. |
| `nh.advertiseService(srv);` | nothing | The constructor registers the service. |
| `ros::ServiceClient<Req, Res> cli("name");` | `r2s::Client<std_srvs::srv::Trigger> cli(node, "name");` | |
| `nh.serviceClient(cli);` | nothing | The constructor registers the call. |
| `cli.call(req, res)` | `cli.call(req, res)` | Same name and meaning: true with the host's answer, false on refusal, timeout or disconnect. |

### Parameters

| Removed | Native | Notes |
| --- | --- | --- |
| `nh.getParam("name", &int)`, `&float`, `&bool`, `&char*` | `r2s_params_get(index)` on a declared table | Look up the entry by index, not by name; see [parameters](#parameters). |
| `getParam`'s `length` and `timeout` arguments | no equivalent | One value per entry, and nothing waits. |
| `nh.getParam` on a parameter the host owns | no equivalent | ros2serial parameters belong to the MCU and the bridge exposes them; the MCU never reads the host's. |

### Logging

| Removed | Native | Notes |
| --- | --- | --- |
| `nh.logdebug(text)`, `loginfo`, `logwarn`, `logerror`, `logfatal` | `R2S_LOG_DEBUG(lit)`, `R2S_LOG_INFO(lit)`, `R2S_LOG_WARN(lit)`, `R2S_LOG_ERROR(lit)`, `R2S_LOG_FATAL(lit)` | The macros keep a literal in flash on AVR. |
| the same calls with a `char *` built at run time | `r2s_log_info_ram(text)` and its siblings | RAM text is a separate call, not an overload. |
| the `F("text")` overloads | `R2S_LOG_INFO("text")` | The macro places the literal itself, so `F()` disappears. |
| a format string with values | `R2S_LOG_INFO_U16(lit, v)`, `r2s_log_info_f32`, and so on | One value per call, typed; the core has no varargs. |

Without `R2S_WITH_LOG` the macros compile out, so one sketch still builds for a
firmware without the channel.

### tf broadcasting

| Removed | Native | Notes |
| --- | --- | --- |
| `tf::TransformBroadcaster bc;` | `r2s::Publisher<tf2_msgs::msg::TFMessage> tf_pub(node, "/tf");` | An ordinary publisher on `/tf`. |
| `bc.init(nh);` | nothing | The constructor announces `/tf`. |
| `bc.sendTransform(transform);` | fill `transforms` and `tf_pub.publish(tf);` | The bundled `TFMessage` holds one transform, so set `size` to 1. |

## 2. The rules that are not a rename

### ROS 1 to ROS 2 message fields

The native types are the ROS 2 definitions, so a ROS 1 sketch's field names
change. `Header` has no `seq`, and a stamp carries `sec` and `nanosec`:

```cpp
#include <stdio.h>

#include <ros2serial.h>

#include <std_msgs/msg/header.hpp>

void fill_frame(std_msgs::msg::Header &msg)
{
    msg.stamp.sec = 12;
    msg.stamp.nanosec = 500000000u;
    (void)snprintf(msg.frame_id, sizeof msg.frame_id, "%s", "base_link");
}
```

An `int64` ROS 1 field that became `int32` in ROS 2 keeps the ROS 2 width. Check
the field against the installed definition when a sketch was written for a
different distro.

### Strings are bounded buffers

A ROS 1 `char *` with a `data_length` becomes `char[N]`, NUL-terminated, and the
bound comes from the bundled type. Copy into it; never assign a pointer:

```cpp
#include <stdio.h>

#include <ros2serial.h>

#include <std_msgs/msg/string.hpp>

void fill_greeting(std_msgs::msg::String &msg, const char *text)
{
    (void)snprintf(msg.data, sizeof msg.data, "%s", text);
}
```

The bundled `String` holds 16 characters plus the terminator, `Header.frame_id`
the same, and a `Trigger` response message the same. `ros2 run r2s_gen r2s-gen
limits` prints every bound with its source, and
[message bounds](message-bounds.md) says how to change one.

### Arrays are bounded, with a count

A ROS 1 pointer plus `data_length` becomes fixed storage plus a `size` field.
Set the count beside the elements, and a count past the storage fails the
publish instead of truncating:

```cpp
#include <stdio.h>

#include <ros2serial.h>

#include <sensor_msgs/msg/joint_state.hpp>

void fill_joints(sensor_msgs::msg::JointState &msg)
{
    msg.position.size = 2u;
    msg.position.data[0] = 1.5;
    msg.position.data[1] = -0.5;
    msg.name.size = 2u;
    (void)snprintf(msg.name.data[0], sizeof msg.name.data[0], "%s", "left");
    (void)snprintf(msg.name.data[1], sizeof msg.name.data[1], "%s", "right");
}
```

`JointState` holds six names and six entries per column. A fixed-width array,
such as the `Imu` covariances, fills element by element and has no size to set.

### Services, parameters and actions need their flag

Each optional channel is a build input, defined in every firmware translation
unit as well as the sketch: `R2S_WITH_SERVICES` for `r2s::Service` and
`r2s::Client`, `R2S_WITH_PARAMS` for the parameter table, `R2S_WITH_ACTIONS`
for `r2s::action::Server`, and `R2S_WITH_LOG` for the log macros. The
[services and parameters](services-and-parameters.md) page carries the exact
compile lines, including the smaller frame limit the services channel needs on
an Uno. A sketch that names an optional channel without its flag fails to
build: `r2s/service.hpp` stops the compile with a message naming the flag.

### The publish result

The old `publish` returned `0` for sent and `-1` for anything else. The native
call returns the status that says which refusal happened, so a ported sketch can
tell a busy link from a rejected topic:

```cpp
#include <ros2serial.h>

#include <std_msgs/msg/float32.hpp>

r2s::Node node("publish_status");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

bool publish_battery(float volts)
{
    std_msgs::msg::Float32 msg;

    msg.data = volts;
    switch (battery_pub.publish(msg)) {
        case R2S_OK:
            return true;
        case R2S_ERR_TX_BUSY:
            return false;
        default:
            return false;
    }
}
```

`R2S_ERR_NOT_CONNECTED` means the handshake has not finished, and
`R2S_ERR_TOPIC_REJECTED` means the host refused the topic or the table already
closed. Do not retry in a loop that starves `spin_some()`.

### Registration happens before the first spin

Every publisher, subscription, service and client registers in its constructor,
and the first `spin_some()` closes the table. A declaration after that is
refused and sets the node's registration error, so a sketch checks it once,
before the ready line:

```cpp
#include <ros2serial.h>

#include <std_msgs/msg/float32.hpp>

r2s::Node node("registration");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

bool ready()
{
    node.spin_some();
    return !node.registration_error();
}
```

File-scope declarations, as in a rosserial sketch, keep the objects alive for
the whole run; a subscription declared inside `loop()` would leave a dangling
registration.

### Time and duration arithmetic

`ros::Time` and `ros::Duration` carried `toSec`, `fromSec`, `toNsec`,
`fromNSec` and the operators. The native types are plain structures, so a port
that did arithmetic turns it into integer nanoseconds:

```cpp
#include <stdint.h>

#include <ros2serial.h>

#include <builtin_interfaces/msg/time.hpp>

int64_t time_ns(const builtin_interfaces::msg::Time &stamp)
{
    return (int64_t)stamp.sec * 1000000000 + (int64_t)stamp.nanosec;
}

builtin_interfaces::msg::Time stamp_between(
    const builtin_interfaces::msg::Time &from, uint32_t offset_ms)
{
    builtin_interfaces::msg::Time out;
    int64_t total = time_ns(from) + (int64_t)offset_ms * 1000000;

    out.sec = (int32_t)(total / 1000000000);
    out.nanosec = (uint32_t)(total % 1000000000);
    return out;
}
```

`r2s_now()` returns the same shared clock the old `nh.now()` read, as a
`r2s_time_t` with `sec` and `nanosec`.

### tf broadcasting

One publisher on `/tf` carrying `tf2_msgs::msg::TFMessage` replaces the
broadcaster. The bundled message holds a single transform:

```cpp
#include <stdio.h>

#include <ros2serial.h>

#include <geometry_msgs/msg/transform_stamped.hpp>
#include <tf2_msgs/msg/tf_message.hpp>

r2s::Node node("tf_demo");
r2s::Publisher<tf2_msgs::msg::TFMessage> tf_pub(node, "/tf");

void broadcast(float x)
{
    tf2_msgs::msg::TFMessage tf;

    tf.transforms.size = 1u;
    tf.transforms.data[0].header.stamp.sec = 1;
    tf.transforms.data[0].header.stamp.nanosec = 0u;
    (void)snprintf(tf.transforms.data[0].header.frame_id,
        sizeof tf.transforms.data[0].header.frame_id, "%s", "base_link");
    (void)snprintf(tf.transforms.data[0].child_frame_id,
        sizeof tf.transforms.data[0].child_frame_id, "%s", "gripper");
    tf.transforms.data[0].transform.translation.x = x;
    tf.transforms.data[0].transform.rotation.w = 1.0;
    tf_pub.publish(tf);
}
```

Publish a `TFMessage` per transform set; the array bound is part of the bundled
type, so a wider set needs a wider bound in your own message definitions.

### Parameters

The MCU declares its table once, before the first spin, and reads the live
values by index. There is no lookup by name and no host timeout:

```cpp
#define R2S_WITH_PARAMS 1

#include <ros2serial.h>

#include <r2s/r2s_params.h>

#include <std_msgs/msg/string.hpp>

static char s_label[17];
static r2s_param_t s_params[2];

r2s::Node node("params_demo");
r2s::Publisher<std_msgs::msg::String> label_pub(node, "label");

void report_param()
{
    const r2s_param_t *led;

    s_params[0].name = "led";
    s_params[0].type = R2S_PARAM_BOOL;
    s_params[0].initial.b = 1u;
    s_params[1].name = "label";
    s_params[1].type = R2S_PARAM_STRING;
    s_params[1].bound = 16u;
    s_params[1].initial.s = "hello";
    s_params[1].value.s = s_label;
    (void)r2s_params_configure(s_params, 2);
    led = r2s_params_get(0);
    if (led != 0 && led->value.b != 0u) {
        R2S_LOG_INFO("led is on");
    }
}
```

`r2s_params_configure` copies each default into its live value, and the session
writes the live value as the bridge delivers a set. The table must outlive the
firmware run, as above.

## 3. One sketch, before and after

The before sketch is a rosserial sketch as the removed layer accepted it: it
publishes `battery`, echoes `cmd_vel` and logs on connect. It is here for
comparison and does not compile against this tree.

<!-- snippet: skip until #701: the removed layer, kept for contrast; no header in this tree accepts it -->
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
    nh.loginfo("boot");
}

void loop()
{
    battery.data = 1.0f;
    battery_pub.publish(&battery);
    nh.spinOnce();
}
```

The after sketch is the same firmware on the native API. The includes, the
node, the two registrations, the log call and the spin change; the topic names
and the publish body do not.

```cpp
#if !defined(ARDUINO)
/* The docs snippet check has no board, so a plain stream stands in for the
 * one the port glue takes on an Arduino. */
struct HostStream
{
    void begin(unsigned long baud)
    {
        (void)baud;
    }
};
HostStream Serial;
#else
#include <Arduino.h>
#endif

#include <ros2serial.h>

#include <geometry_msgs/msg/twist.hpp>
#include <std_msgs/msg/float32.hpp>

r2s::Node node("battery_node");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    (void)msg;
}
r2s::Subscription<geometry_msgs::msg::Twist> cmd_vel_sub(node, "cmd_vel", on_cmd_vel);

void setup()
{
    node.begin(Serial, 115200);
    R2S_LOG_INFO("boot");
}

void loop()
{
    std_msgs::msg::Float32 battery;

    battery.data = 1.0f;
    battery_pub.publish(battery);
    node.spin_some();
}
```

The sketch is now named where the old one announced as `rosserial`: the bridge
shows `battery_node`, and the topics keep their names. Porting several sketches
onto one board's link would collide, so keep one node name per firmware.

## 4. Prove the port builds

Stage the library and compile the sketch with the shipped helper, from the ROS
workspace root:

```sh
ros2 run r2s_gen r2s-arduino library --out firmware/ros2serial
ros2 run r2s_gen r2s-arduino compile \
  --library firmware/ros2serial --fqbn arduino:avr:uno \
  --out firmware/build firmware/battery_node
```

The helper reads the frame limit and the channel flags from the staged library,
so a topic-only port needs nothing else. A port that also serves a service or
declares parameters passes its `R2S_WITH_*` flag and the smaller frame limit as
the [services and parameters](services-and-parameters.md) page shows, then
uploads with Arduino CLI as in the [README](../README.md) walkthrough.

If the sketch compiles and the bridge reports the handshake, the port is done:
`ros2 topic echo /battery std_msgs/msg/Float32 --once` prints the samples the
old sketch sent.
