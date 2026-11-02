# ros2serial

rosserial for ROS 2: publish and subscribe to ROS 2 topics from small microcontrollers, down to an 8-bit Arduino Uno, over a serial link.

**Status:** pre-release. The stack runs end to end; the walkthrough below takes an Uno (real or emulated) to `ros2 topic echo`. The design is in [`docs/00-design.md`](docs/00-design.md), and the work in the [issues](https://github.com/apojomovsky/ros2serial/issues).

## What it will look like

```cpp
#define R2S_NODE_NAME "base_controller"
#define R2S_TOPICS(X) \
  X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
  X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>
```

Install the Arduino library, declare your topics, publish, call spin. On the host, a regular ROS 2 node bridges the serial port to any RMW.

## Getting started

Publish your first topic from an Uno. The firmware uses the runtime API:
topics are objects declared in the sketch, so there is no `r2s.yaml` and
no generated code. The example sketch is
`examples/arduino_minimal/arduino_minimal.ino`: a node called
`base_controller` with a `battery` publisher and a `cmd_vel` subscription,
and a one-line loop:

```text
r2s::Node node("base_controller");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
void loop()  { battery.data = read_battery(); battery_pub.publish(battery); node.spin_some(); }
```

You need ROS 2 Jazzy with `colcon`, `arduino-cli` with the AVR core and
`avr-gcc`, Python 3.10 or newer with `pip`, and libsimavr for the emulated
run. Every command below also runs in CI against simavr, so the steps
cannot drift from the code.

### 1. Install the generator

```sh
git clone https://github.com/apojomovsky/ros2serial.git
cd ros2serial
pip install ./gen
r2s-gen version
```

### 2. Install the Arduino library

The library is the generator bundle plus the core, the Arduino port glue
and the runtime layer:

```sh
LIB=~/Arduino/libraries/ros2serial
mkdir -p "$LIB/src/r2s"
r2s-gen bundle --distro jazzy --out "$LIB"
cp core/include/r2s/*.h "$LIB/src/r2s/"
cp core/src/*.c "$LIB/src/"
rm -f "$LIB/src/test_reset.c"
cp ports/arduino/r2s_arduino_port.cpp ports/arduino/r2s_arduino_port.h "$LIB/src/"
cp cpp/include/r2s/runtime.hpp "$LIB/src/r2s/"
cp cpp/src/runtime.cpp "$LIB/src/"
arduino-cli core install arduino:avr
```

### 3. Build the bridge

```sh
source /opt/ros/jazzy/setup.bash
colcon build --base-paths bridge --build-base build/bridge/build --install-base build/bridge/install
source build/bridge/install/setup.bash
```

Build outputs land under `build/`, which the checkout ignores.

### 4. Compile the example

The sketch's frame and nesting limits travel as flags, read from the
bundle the same way the example builds read them:

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
  --library "$LIB" --output-dir build/uno examples/arduino_minimal
```

### 5. Run it on the emulated Uno

Build the simavr harness and start the firmware on it. It prints the pty
the emulated UART is bridged to:

```sh
R2S_BUILD_DIR=$PWD/build bash tests/harness/simavr/run.sh --build-only
build/harness/simavr/r2s_simavr --mcu atmega328p --firmware build/uno/arduino_minimal.ino.elf --run-ms 300000
```

```text
r2s_simavr: pty=/dev/pts/0
```

### 6. Bridge it into ROS 2

In a second shell, with the ROS and bridge environments sourced, launch
the bridge on that pty and watch the topic:

```sh
source /opt/ros/jazzy/setup.bash
source build/bridge/install/setup.bash
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/pts/0
```

```sh
ros2 topic list
```

```text
/battery
/cmd_vel
/diagnostics
/parameter_events
/rosout
```

```sh
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

```text
data: 0.0
```

### 7. On real hardware

Flash the same build to an Uno and point the bridge at the serial port
instead of the pty:

```console
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno --input-dir build/uno examples/arduino_minimal
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

The Uno resets when its serial port opens, then runs its bootloader before
your sketch starts. The bridge waits this out: `boot_wait_ms` in
`bridge/r2s_bridge/config/r2s_bridge.params.yaml` defaults to 2500 ms,
which covers the reset and the bootloader. If your board enumerates
differently, substitute its port (often `/dev/ttyUSB0`) for `/dev/ttyACM0`
in both commands.

## Contributing

New here? Start with the quickstart in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

MIT, see [`LICENSE`](LICENSE).
