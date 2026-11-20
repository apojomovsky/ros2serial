# Choosing how to declare your topics

On an Arduino board (Uno, Mega, ESP32) your C++ sketch declares its topics in
one of two ways. Both put the same bytes on the wire, both are served by the
same bridge, and both are exercised by the end-to-end suite. They differ in how
the declarations read and in what they cost in flash, RAM and stack.

> **Building in C, or without the Arduino library?** Read [Building in C or
> without the Arduino library](c-and-bare-metal.md) instead. The runtime objects
> are C++ only.

## The short answer

Start with the **runtime objects**: `r2s::Node`, `r2s::Publisher<T>` and
`r2s::Subscription<T>`. They are what the quickstart and the firmware guide use,
and they read like ROS 2 code.

If your sketch runs short of RAM, move the declarations into an **X-macro
list** (`R2S_TOPICS(X)`). The body of the sketch does not change. On the
measured two-topic Uno example it saves 418 of 1256 bytes of RAM (33 %) and
926 of 11198 bytes of flash (8 %), from [size and stack
budgets](budgets.md).

Coming from rosserial? The [migration guide](03-port-rosserial-sketch.md) comes
first, and it lands on the runtime objects.

## Pick by situation

| Your situation | Use | Why |
| --- | --- | --- |
| First sketch on an Uno, Mega or ESP32 | Runtime objects | Shortest sketch, and the API the guides teach |
| An Uno sketch short of RAM | X-macro list | Same sketch body, no RAM topic table |
| Services on the MCU | Runtime objects | A service has no list row |
| Actions on the MCU | Either | `r2s::action::Server`, or an `R2S_ACTIONS` row |
| Per-topic QoS (`keep_latest`, `reliable`) | Runtime objects | An X-macro topic always announces best effort |
| Your own `.msg`, `.srv` or `.action` types | Either | Both name a library that `r2s-gen types` emits (see [custom interfaces](custom-interfaces.md)) |

## The same sketch, both ways

A node `base_controller` publishes `battery` (`std_msgs/msg/Float32`) and
subscribes to `cmd_vel` (`geometry_msgs/msg/Twist`).

### Runtime objects

This is `examples/arduino_minimal`:

```cpp
#include <ros2serial.h>
#include <std_msgs/msg/float32.hpp>
#include <geometry_msgs/msg/twist.hpp>

/* Stand-ins for your hardware and for the board's Serial. */
static void set_motors(double linear, double angular) { (void)linear; (void)angular; }
static float read_battery(void) { return 0.0f; }
struct Stream { void begin(unsigned long baud) { (void)baud; } };
Stream Serial;

r2s::Node node("base_controller");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    set_motors(msg.linear.x, msg.angular.z);
}
r2s::Subscription<geometry_msgs::msg::Twist> cmd_vel_sub(node, "cmd_vel", on_cmd_vel);

std_msgs::msg::Float32 battery;

void setup() { node.begin(Serial, 115200); }

void loop()
{
    battery.data = read_battery();
    battery_pub.publish(battery);
    node.spin_some();
}
```

### X-macro list

`examples/arduino_static`, the same sketch. The two declaration lines become
rows above the include, and everything below them is unchanged:

```cpp
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>

/* Stand-ins for your hardware and for the board's Serial. */
static void set_motors(double linear, double angular) { (void)linear; (void)angular; }
static float read_battery(void) { return 0.0f; }
struct Stream { void begin(unsigned long baud) { (void)baud; } };
Stream Serial;

r2s::Node node("base_controller");

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    set_motors(msg.linear.x, msg.angular.z);
}

std_msgs::msg::Float32 battery;

void setup() { node.begin(Serial, 115200); }

void loop()
{
    battery.data = read_battery();
    battery_pub.publish(battery);
    node.spin_some();
}
```

The header expands the rows into the topic table, one typed publish function
per publisher, and the dispatch that calls each subscription callback. The
library's own sources never see the list.

## What each one costs and gives up

| | Runtime objects | X-macro list |
| --- | --- | --- |
| Where the topics live | A RAM table the firmware fills at run time | A constant table in flash |
| RAM on the measured two-topic Uno sketch | 1256 bytes | 838 bytes |
| Flash on the same sketch | 11198 bytes | 10272 bytes |
| Stack on the same sketch | 173 bytes | 217 bytes (the reason is not established) |
| Services | `r2s::Service`, `r2s::Client` | No row form |
| Actions | `r2s::action::Server` | `R2S_ACTIONS` rows from `<r2s/action.hpp>` |
| Per-topic QoS | A constructor argument | Always best effort |
| Registration | All topics before the first spin; a later one sets the node's registration error | Fixed at compile time |

Every figure in the table comes from [size and stack budgets](budgets.md)
(`arduino_minimal` against `arduino_static`), measured on an Uno.

The percentage applies to this two-topic example only. The runtime route's
topic table is a fixed cost, so it shrinks, not grows, as your own sketch does
more work.

## How many topics fit

Capacity figures per board are being measured (a table of the fixed library
cost, the cost of each publisher, subscription and service, and how many fit on
an Uno and a Mega for a stated allowance of your own code). Until that table
exists, this page states no number it cannot source.

These rules hold today, from the [design](00-design.md) and the frame-limit
section of the [firmware guide](firmware.md#the-frame-limit-is-a-build-input):

- The receive buffer costs about one byte of RAM per byte of the frame limit,
  plus one byte per 254.
- Services keep three frame-sized buffers, so a byte of frame limit costs more
  when a service is in the build.
- The runtime route's table is sized for its topic limit (16 entries by
  default), so its RAM floor does not depend on how many topics you register.
  Each entry is 14 bytes on an ATmega328P.

## Switching later

The bridge needs no change. The firmware announces its topic names and types
when it connects, so the ROS graph looks the same after a switch.

`arduino_minimal` and `arduino_static` are the same sketch in both forms, so
switching is a change of declaration lines. Two things do not survive the move
in either direction:

- A topic declared with a QoS argument has no row form, so it announces best
  effort after the switch.
- A service has no row form at all, so it stays a runtime object.

## Where to go next

- Add your first topic: [the firmware guide](firmware.md).
- Bring your own message, service or action: [custom
  interfaces](custom-interfaces.md).
- Build in C, with CMake or PlatformIO, or on a board of your own: [Building
  in C or without the Arduino library](c-and-bare-metal.md).
- Check what one more topic costs in bytes: [size and stack budgets](budgets.md).
