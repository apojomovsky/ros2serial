#!/usr/bin/env bash
# Syntax-only compile of the C++ layer's headers with avr-g++, so the code
# that reaches a board is checked against the toolchain that builds it.
# The message headers include the bundle's C header, so one is staged where
# they find it; nothing is written to the tree.
# Usage: avr-syntax.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 2
build=${R2S_BUILD_DIR:-$HOME/.cache/ros2serial/build$(echo "$root" | tr / -)}/cpp/avr-syntax
mkdir -p "$build" || exit 1

if ! command -v avr-g++ >/dev/null 2>&1; then
  echo "avr-g++ not found: run this in the dev image"
  exit 1
fi

flags=(-mmcu=atmega328p -std=gnu++11 -Os -Wall -Wextra -Werror -Wconversion -Wshadow
  -fno-exceptions -fno-rtti -I "$root/core/include" -I "$root/cpp/include")

# The bundle's C header, where the message headers look for it. The tree
# carries the frozen copy the generator emits; the golden file stands in
# when it is not there yet, so this check does not depend on the generator
# running.
bundled="$root/cpp/include/r2s_bundled.h"
if [ ! -f "$bundled" ]; then
  bundled="$root/gen/tests/golden/bundle/r2s_bundled.h"
fi
if [ ! -f "$bundled" ]; then
  echo "avr-syntax: no r2s_bundled.h to compile the message headers against"
  exit 1
fi
stage="$build/stage"
rm -rf "$stage"
mkdir -p "$stage"
cp "$bundled" "$stage/" || exit 1
flags+=(-I "$stage")

# One translation unit per header, so a header that breaks the toolchain
# fails here and names itself. The header is included by the path a sketch
# uses, with the package directory on the include path.
sources=()
for header in "$root"/cpp/include/*/msg/*.hpp "$root"/cpp/include/*/srv/*.hpp; do
  [ -e "$header" ] || continue
  name=$(basename "$header" .hpp)
  package=$(basename "$(dirname "$(dirname "$header")")")
  printf '#include <%s/msg/%s.hpp>\n' "$package" "$name" >"$build/check_$name.cpp"
  sources+=("$build/check_$name.cpp")
done
# The runtime header on its own, without a message type, so a change there
# fails even when no message header exists yet.
printf '#include "r2s/runtime.hpp"\n' >"$build/check_runtime.cpp"
sources+=("$build/check_runtime.cpp")
# A static sketch: the rows expand to the global objects through the bundled
# header, proving the whole static path in the epic-cc subset.
cat >"$build/check_static_sketch.cpp" <<'EOF'
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>
void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    (void)msg;
}
EOF
sources+=("$build/check_static_sketch.cpp")
# An action server sketch, runtime and static: the Fibonacci-like action
# rides on the bundled Float32 descriptors for its goal, result and
# feedback, proving both declaration paths compile for the board. The
# static list expands after the traits and callbacks it names, since the
# rows instantiate the server.
cat >"$build/check_action_sketch.cpp" <<'EOF'
#include <std_msgs/msg/float32.hpp>
#include <r2s/action.hpp>
struct Spin {
    using Goal = std_msgs::msg::Float32;
    using Result = std_msgs::msg::Float32;
    using Feedback = std_msgs::msg::Float32;
};
namespace r2s
{
namespace action
{
template <> struct traits<Spin> {
    static const char *action_name()
    {
        return "spin";
    }
    static const char *type_name()
    {
        return "test_msgs/action/Fibonacci";
    }
    static const uint8_t *type_hash()
    {
        static const uint8_t hash[8] = {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};
        return hash;
    }
};
} // namespace action
} // namespace r2s
r2s::action::GoalResponse on_spin_goal(const std_msgs::msg::Float32 &goal)
{
    (void)goal;
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}
r2s::action::CancelResponse on_spin_cancel(const std_msgs::msg::Float32 &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::ACCEPT;
}
void on_spin_accepted(r2s::action::ServerGoalHandle<Spin> &handle)
{
    std_msgs::msg::Float32 feedback;
    feedback.data = 1.0f;
    (void)handle.publish_feedback(feedback);
}
r2s::action::Server<Spin> fib("spin", on_spin_goal, on_spin_cancel, on_spin_accepted);
EOF
cat >"$build/check_action_static.cpp" <<'EOF'
#include <std_msgs/msg/float32.hpp>
#include <r2s/action.hpp>
struct StaticSpin {
    using Goal = std_msgs::msg::Float32;
    using Result = std_msgs::msg::Float32;
    using Feedback = std_msgs::msg::Float32;
};
namespace r2s
{
namespace action
{
template <> struct traits<StaticSpin> {
    static const char *action_name()
    {
        return "spin";
    }
    static const char *type_name()
    {
        return "test_msgs/action/Fibonacci";
    }
    static const uint8_t *type_hash()
    {
        static const uint8_t hash[8] = {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};
        return hash;
    }
};
} // namespace action
} // namespace r2s
r2s::action::GoalResponse on_static_goal(const std_msgs::msg::Float32 &goal)
{
    (void)goal;
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}
r2s::action::CancelResponse on_static_cancel(const std_msgs::msg::Float32 &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::REJECT;
}
void on_static_accepted(r2s::action::ServerGoalHandle<StaticSpin> &handle)
{
    (void)handle;
}
#define R2S_ACTIONS(X) X(StaticSpin, static_fib, "spin", on_static_goal, on_static_cancel, on_static_accepted)
R2S_ACTIONS(R2S_ACTION_X_SERVER)
EOF
action_sources=("$build/check_action_sketch.cpp" "$build/check_action_static.cpp")
# The headers as a sketch includes them, with the package directories on
# the path, so the spelling a sketch uses is the one that compiles.
printf '#include <std_msgs/msg/float32.hpp>\n#include <geometry_msgs/msg/twist.hpp>\n' \
  >"$build/check_sketch.cpp"
sources+=("$build/check_sketch.cpp")
# A rosserial-style sketch over the compatibility headers, proving the
# spelling a ported sketch uses, with the size parameters ros_lib sketches
# carry. The member-function arity rides along, so both subscribe forms
# compile for the board.
cat >"$build/check_ros_sketch.cpp" <<'EOF'
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
ros::NodeHandle_<ros::ArduinoHardware, 1, 1, 64, 64> nh_small;
ros::Time stamp;
ros::Duration timeout(1, 0);
void setup()
{
    nh.initNode();
    nh.advertise(battery_pub);
    nh.subscribe(cmd_vel_sub);
    nh.subscribe(echo_sub);
    nh.setSpinTimeout(100u);
    (void)nh.getHardware();
}
void loop()
{
    battery.data = 1.0f;
    battery_pub.publish(&battery);
    (void)nh.spinOnce();
    (void)nh.connected();
    stamp = nh.now();
    stamp += timeout;
    nh.logdebug("dbg");
    nh.loginfo("inf");
    nh.logwarn("wrn");
    nh.logerror("err");
    nh.logfatal("ftl");
}
EOF
sources+=("$build/check_ros_sketch.cpp")
# The same sketch for the Arduino build, so initNode's Serial branch
# compiles too. The stub stands in for the core's stream type, and the
# syntax check needs no link.
cat >"$stage/Arduino.h" <<'EOF'
#ifndef R2S_AVR_SYNTAX_ARDUINO_H
#define R2S_AVR_SYNTAX_ARDUINO_H
class Stream
{
};
class HardwareSerial : public Stream
{
  public:
    void begin(unsigned long baud)
    {
        (void)baud;
    }
};
extern HardwareSerial Serial;
// The flash-string helper and macro the core's F() sketches use, spelled
// the way WString.h spells them, so the log overloads below resolve.
class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))
#endif
EOF
cat >"$build/check_ros_arduino.cpp" <<'EOF'
#include <Arduino.h>
#include <ros2serial/ros.h>
#include <ros2serial/std_msgs/Float32.h>
ros::NodeHandle nh;
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);
void setup()
{
    nh.initNode();
    nh.advertise(battery_pub);
    nh.loginfo(F("boot"));
}
void loop()
{
    battery_pub.publish(&battery);
    nh.spinOnce();
    nh.loginfo(F("loop"));
}
EOF
# The porting guide's one-line edits, compiling for the board: strings copy
# into fixed buffers, bounded arrays set their size beside the elements, and
# the header fills its stamp and frame id. The tf broadcaster keeps its
# ros_lib shape, advertising /tf and sending the stamped transform.
cat >"$build/check_ros_edits.cpp" <<'EOF'
#include <ros2serial/ros.h>
#include <ros2serial/geometry_msgs/TransformStamped.h>
#include <ros2serial/sensor_msgs/Imu.h>
#include <ros2serial/sensor_msgs/JointState.h>
#include <ros2serial/std_msgs/Header.h>
#include <ros2serial/std_msgs/String.h>
#include <ros2serial/tf/transform_broadcaster.h>
#include <stdio.h>
ros::NodeHandle nh;
tf::TransformBroadcaster broadcaster;
sensor_msgs::JointState joints;
std_msgs::String greeting;
std_msgs::Header frame;
sensor_msgs::Imu imu;
void fill_greeting(std_msgs::String &msg, const char *text)
{
    (void)snprintf(msg.data, sizeof msg.data, "%s", text);
}
void fill_joints(sensor_msgs::JointState &msg)
{
    msg.position.size = 2u;
    msg.position.data[0] = 1.5;
    msg.position.data[1] = -0.5;
    msg.name.size = 2u;
    (void)snprintf(msg.name.data[0], sizeof msg.name.data[0], "%s", "left");
    (void)snprintf(msg.name.data[1], sizeof msg.name.data[1], "%s", "right");
}
void fill_frame(std_msgs::Header &msg)
{
    msg.stamp.sec = 12;
    msg.stamp.nanosec = 500000000u;
    (void)snprintf(msg.frame_id, sizeof msg.frame_id, "%s", "base_link");
}
void setup()
{
    nh.initNode();
    broadcaster.init(nh);
    imu.orientation_covariance[0] = 0.0;
    fill_greeting(greeting, "hello");
    fill_joints(joints);
    fill_frame(frame);
}
void loop()
{
    geometry_msgs::TransformStamped transform;

    transform.header.stamp.sec = 1;
    transform.header.stamp.nanosec = 0u;
    (void)snprintf(transform.header.frame_id, sizeof transform.header.frame_id, "%s", "base");
    (void)snprintf(transform.child_frame_id, sizeof transform.child_frame_id, "%s", "gripper");
    transform.transform.translation.x = 1.0;
    transform.transform.rotation.w = 1.0;
    broadcaster.sendTransform(transform);
    (void)nh.spinOnce();
}
EOF
sources+=("$build/check_ros_edits.cpp")

status=0
for source in "${sources[@]}"; do
  if ! avr-g++ "${flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source did not compile"
    status=1
  fi
done
# A service sketch, runtime and compat: the Trigger service rides on the
# bundled tables, proving both declaration paths compile for the board.
# The getParam calls ride along without the parameters channel, so a
# sketch using them builds for every tier.
cat >"$build/check_service_sketch.cpp" <<'EOF'
#include <std_srvs/srv/trigger.hpp>
#include <r2s/service.hpp>
r2s::Node node("svc");
void on_reset(const std_srvs::srv::Trigger::Request &req, std_srvs::srv::Trigger::Response &res)
{
    (void)req;
    res.success = 1u;
    res.message[0] = '\0';
}
r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);
r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
bool call_trigger()
{
    std_srvs::srv::Trigger::Request req;
    std_srvs::srv::Trigger::Response res;
    return trigger.call(req, res);
}
EOF
cat >"$build/check_ros_service.cpp" <<'EOF'
#include <ros2serial/ros.h>
#include <ros2serial/std_srvs/Trigger.h>
ros::NodeHandle nh;
void on_reset(const std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
{
    (void)req;
    res.success = 1u;
    res.message[0] = '\0';
}
ros::ServiceServer<std_srvs::Trigger::Request, std_srvs::Trigger::Response> reset_srv("reset",
    &on_reset);
class Adder
{
  public:
    void on_add(const std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
    {
        (void)req;
        res.success = 1u;
    }
};
Adder adder;
ros::ServiceServer<std_srvs::Trigger::Request, std_srvs::Trigger::Response, Adder> add_srv("add",
    &Adder::on_add, &adder);
ros::ServiceClient<std_srvs::Trigger::Request, std_srvs::Trigger::Response> trigger_cli(
    "trigger");
void setup()
{
    int count = 0;
    float gain = 0.0f;
    char text[32];
    char *slot = text;
    bool flag = false;
    nh.initNode();
    nh.advertiseService(reset_srv);
    nh.advertiseService(add_srv);
    nh.serviceClient(trigger_cli);
    (void)nh.getParam("count", &count);
    (void)nh.getParam("gain", &gain);
    (void)nh.getParam("label", &slot);
    (void)nh.getParam("flag", &flag);
}
void loop()
{
    std_srvs::Trigger::Request req;
    std_srvs::Trigger::Response res;
    (void)trigger_cli.call(req, res);
    (void)nh.spinOnce();
}
EOF
service_sources=("$build/check_service_sketch.cpp" "$build/check_ros_service.cpp")
# The service sketches and the registry source, with the services channel
# on: the channel is a build flag, so these prove the flag the sketches
# build with.
service_flags=("${flags[@]}" -DR2S_WITH_SERVICES)
for source in "${service_sources[@]}" "$root/cpp/src/service.cpp"; do
  if ! avr-g++ "${service_flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source did not compile"
    status=1
  fi
done
# The runtime's own source, since a board compiles it as well. The AVR
# toolchains carry no C++ standard library, so this catches a header the
# host build accepts and the board does not.
if ! avr-g++ "${flags[@]}" -fsyntax-only "$root/cpp/src/runtime.cpp"; then
  echo "avr-syntax: cpp/src/runtime.cpp did not compile"
  status=1
fi
# The action sketches and the registry source, with the actions channel on:
# the channel is a build flag, so these prove the flag the sketches build
# with.
action_flags=("${flags[@]}" -DR2S_WITH_ACTIONS)
for source in "${action_sources[@]}" "$root/cpp/src/action.cpp"; do
  if ! avr-g++ "${action_flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source did not compile"
    status=1
  fi
done
# The Arduino build of the rosserial sketch, so initNode's Serial branch
# compiles against the same stub the TU above stages. The port glue lives
# beside the sketch on a board, so its directory rides along here.
if ! avr-g++ "${flags[@]}" -fsyntax-only -DARDUINO -I "$root/ports/arduino" "$build/check_ros_arduino.cpp"; then
  echo "avr-syntax: $build/check_ros_arduino.cpp did not compile"
  status=1
fi
[ "$status" = 0 ] || exit 1
echo "avr-syntax ok: ${#sources[@]} translation units, ${#action_sources[@]} action sketches, ${#service_sources[@]} service sketches and the runtime sources"
