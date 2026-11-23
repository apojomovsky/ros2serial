#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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
  -fno-exceptions -fno-rtti -I "$root/core/include" -I "$root/cpp/include"
  -DR2S_MAX_FRAME=325)

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
  kind=$(basename "$(dirname "$header")")
  printf '#include <%s/%s/%s.hpp>\n' "$package" "$kind" "$name" >"$build/check_$name.cpp"
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
# The stub stands in for the core's stream type, so the Arduino build below
# resolves Serial and the flash strings the log macros read.
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
// The flash-string helper WString.h declares, which the sketch header
// reaches on AVR.
class __FlashStringHelper;
#endif
EOF
cat >"$build/check_migrated_arduino.cpp" <<'EOF'
#include <Arduino.h>
#include <stdio.h>
#include <ros2serial.h>
#include <geometry_msgs/msg/twist.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/header.hpp>
#include <std_msgs/msg/string.hpp>
#include <tf2_msgs/msg/tf_message.hpp>
r2s::Node node("migrated");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
r2s::Publisher<tf2_msgs::msg::TFMessage> tf_pub(node, "/tf");
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
    std_msgs::msg::String text;
    std_msgs::msg::Header frame;
    tf2_msgs::msg::TFMessage tf;

    battery.data = 1.0f;
    battery_pub.publish(battery);
    (void)snprintf(text.data, sizeof(text.data), "%s", "hello");
    frame.stamp.sec = 1;
    (void)snprintf(frame.frame_id, sizeof(frame.frame_id), "%s", "base_link");
    tf.transforms.size = 1u;
    tf.transforms.data[0].child_frame_id[0] = '\0';
    tf_pub.publish(tf);
    node.spin_some();
}
EOF

status=0
for source in "${sources[@]}"; do
  if ! avr-g++ "${flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source did not compile"
    status=1
  fi
done
# A service sketch: the Trigger service rides on the bundled tables, so
# the declaration path a firmware uses compiles for the board.
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
service_sources=("$build/check_service_sketch.cpp")
# The service sketch and the registry source, with the services channel on:
# the channel is a build flag, so these prove the flag the sketch builds
# with.
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
# The IDE route: the channel headers read a staged r2s_config.h before their
# #error, so a sketch including one of them first builds with no flag.
config_stage="$build/config_stage"
rm -rf "$config_stage"
mkdir -p "$config_stage"
printf '#define R2S_WITH_SERVICES\n#define R2S_WITH_ACTIONS\n' >"$config_stage/r2s_config.h"
cat >"$build/check_config_first.cpp" <<'EOF'
#include <r2s/service.hpp>
#include <r2s/action.hpp>
int main()
{
    return 0;
}
EOF
if ! avr-g++ "${flags[@]}" -I "$config_stage" -fsyntax-only "$build/check_config_first.cpp"; then
  echo "avr-syntax: the channel headers ignore a staged r2s_config.h"
  status=1
fi
# The registry sources guard on the same flag, so they compile against the
# staged config too: the sketch links them with no flag of its own.
for source in "$root/cpp/src/service.cpp" "$root/cpp/src/action.cpp"; do
  if ! avr-g++ "${flags[@]}" -I "$config_stage" -I "$root/cpp/include" -fsyntax-only "$source"; then
    echo "avr-syntax: $source ignores a staged r2s_config.h"
    status=1
  fi
done
# Without the flag and without the config the headers still refuse, so a
# topics-only build never pays for the channels.
cat >"$build/check_service_noflag.cpp" <<'EOF'
#include <r2s/service.hpp>
int main()
{
    return 0;
}
EOF
cat >"$build/check_action_noflag.cpp" <<'EOF'
#include <r2s/action.hpp>
int main()
{
    return 0;
}
EOF
for source in "$build/check_service_noflag.cpp" "$build/check_action_noflag.cpp"; do
  if avr-g++ "${flags[@]}" -fsyntax-only "$source"; then
    echo "avr-syntax: $source compiled without its channel flag"
    status=1
  fi
done
# The Arduino build of the migration guide's after sketch, so the native
# API's Serial branch and its flash-string log compile for the board. The
# log channel is a build flag, so the sketch that logs passes it. The port
# glue lives beside the sketch on a board, so its directory rides along.
if ! avr-g++ "${flags[@]}" -fsyntax-only -DARDUINO -DR2S_WITH_LOG -I "$root/ports/arduino" "$build/check_migrated_arduino.cpp"; then
  echo "avr-syntax: $build/check_migrated_arduino.cpp did not compile"
  status=1
fi
[ "$status" = 0 ] || exit 1
echo "avr-syntax ok: ${#sources[@]} translation units, ${#action_sources[@]} action sketches, ${#service_sources[@]} service sketches and the runtime sources"
