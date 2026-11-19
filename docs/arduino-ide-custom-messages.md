# Custom messages in the Arduino IDE

This walks one small message of your own from its definition to a board
publishing it, for a reader who works in the Arduino IDE. The type comes from a
ROS interface package, the generated library goes in through *Add .ZIP
Library*, and the bridge loads the same package on the host.

The blocks marked below are the walkthrough the check runs: they are written
and executed in order on a clean home directory, and the sketch is compiled
against the archive this page builds. If a command here drifts from the code,
the check fails. Which route carries a custom type, and what each one needs, is
on [The library route and its config boundary](type-library.md); a field's
storage is on [message bounds](message-bounds.md).

## 1. Define the interface package

The MCU and the host must agree on the message, so the definition lives in a ROS
package beside your other interfaces. Create it under your workspace:

<!-- docs-walkthrough: file ros2_ws/src/my_robot_interfaces/msg/SensorReading.msg -->
```text
float32 temperature
uint16 status
```

<!-- docs-walkthrough: file ros2_ws/src/my_robot_interfaces/package.xml -->
```text
<?xml version="1.0"?>
<package format="3">
  <name>my_robot_interfaces</name>
  <version>0.1.0</version>
  <description>Interfaces for my robot.</description>
  <maintainer email="you@example.com">You</maintainer>
  <license>MIT</license>
  <buildtool_depend>ament_cmake</buildtool_depend>
  <buildtool_depend>rosidl_default_generators</buildtool_depend>
  <exec_depend>rosidl_default_runtime</exec_depend>
  <member_of_group>rosidl_interface_packages</member_of_group>
  <export>
    <build_type>ament_cmake</build_type>
  </export>
</package>
```

<!-- docs-walkthrough: file ros2_ws/src/my_robot_interfaces/CMakeLists.txt -->
```text
cmake_minimum_required(VERSION 3.22)
project(my_robot_interfaces)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME} "msg/SensorReading.msg")

ament_export_dependencies(rosidl_default_runtime)
ament_package()
```

Build it with a ROS 2 environment sourced, and confirm the host knows the
message:

<!-- docs-walkthrough: run -->
```sh
cd ~/ros2_ws
colcon build --packages-up-to my_robot_interfaces
source install/setup.bash
ros2 interface show my_robot_interfaces/msg/SensorReading
```

## 2. Generate the library and its archive

`r2s-gen types` reads the definition from the sourced workspace and writes a
library whose sketch header, type hashes and frame limit come from this
message. `--zip` adds the archive the IDE installs, with the core, the C++
layer and the Arduino port staged inside it, so the build needs no property:

<!-- docs-walkthrough: run -->
```sh
source ~/ros2_ws/install/setup.bash
mkdir -p ~/firmware
cd ~/firmware
ros2 run r2s_gen r2s-gen types my_robot_interfaces/msg/SensorReading \
  --out lib --zip dist/sensor_types.zip
```

Its single top folder is named from the type list, so a second library installs
beside it instead of replacing it. The out directory holds the same files
unarchived, which is what you review; [The library route and its config
boundary](type-library.md#the-archive-the-arduino-ide-installs) says what the
archive carries and why the limit travels inside it.

## 3. Write the sketch

The sketch names the message type the archive carries, so it includes nothing
but the sketch header. A publisher repeats its reading so a subscriber that
starts late still sees one:

<!-- docs-walkthrough: file firmware/sensor_sketch/sensor_sketch.ino -->
```text
#include <ros2serial.h>

r2s::Node node("sensor_board");
r2s::Publisher<my_robot_interfaces::msg::SensorReading> reading_pub(node, "reading");

static uint16_t s_status;

void setup()
{
    node.begin(Serial, 115200);
}

void loop()
{
    static uint32_t last_pub;

    if (node.connected() && millis() - last_pub >= 100u) {
        my_robot_interfaces::msg::SensorReading reading;

        reading.temperature = (float)s_status / 10.0f;
        reading.status = s_status;
        if (reading_pub.publish(reading) == R2S_OK) {
            s_status++;
            last_pub = millis();
        }
    }
    node.spin_some();
}
```

The sketch directory and the `.ino` carry the same name, as the IDE requires.

## 4. Install the archive in the IDE

In the IDE, choose *Sketch > Include Library > Add .ZIP Library* and pick
`~/firmware/dist/sensor_types.zip`. Its types then resolve like bundled ones
and the sketch above compiles with no board property, no `r2s_config.h` edit
and no extra library. Open the sketch, select your board and port under
*Tools*, and press Upload.

The same two steps on a command line, which is what the check runs in place of
the IDE:

<!-- docs-walkthrough: run -->
```sh
cd ~/firmware
python3 -m zipfile -e dist/sensor_types.zip ide
arduino-cli compile --fqbn arduino:avr:uno \
  --library ide/ros2serial_types_* --output-dir build sensor_sketch
```

## 5. Read the topic

With the board flashed, launch the bridge in a shell where your workspace is
sourced, so the host can load the message type the firmware announces:

<!-- docs-walkthrough: serve -->
```sh
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0
```

The Uno resets when its port opens; allow the bootloader and handshake to
finish before expecting data. Substitute your own port, for a Mega or a
different enumeration, in this one argument. In a second sourced shell, ask for
one reading:

<!-- docs-walkthrough: live -->
```sh
ros2 topic echo /reading my_robot_interfaces/msg/SensorReading --once
```

```text
temperature: 1.5
status: 15
---
```

Both fields come from the same payload, so a reading proves the type's layout
survived the whole path.

## After a `.msg` change

The type hashes travel inside the library, so run step 2 again after editing the
definition, install the new archive, and rebuild the sketch. A stale archive
compiles cleanly and then makes the bridge reject every topic of that type at
handshake; [staleness](type-library.md#staleness-regenerate-after-a-msg-change)
says why.

## Where to go next

- Generate for another toolchain, or drive a service or an action:
  [custom interfaces](custom-interfaces.md).
- Pick between the runtime objects and the X-macro list: [declaring
  topics](declaring-topics.md).
- Try the same path without a board: [without hardware](without-hardware.md).
