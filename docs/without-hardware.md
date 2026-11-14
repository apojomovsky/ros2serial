# Develop without hardware

For a real board, start with the [native ROS walkthrough](../README.md).
These optional development paths exercise the same core and bridge.
Run their commands inside the contributor image opened with `make shell`;
it supplies ROS, Arduino CLI, the AVR compiler and simavr. See
[Contributing](../CONTRIBUTING.md) for setup.

## Emulated Uno

No hardware needed for the first pass: the sketch below runs on an emulated Uno, and the hardware section flashes the same build to a real one. The firmware runs a `battery` publisher and a `cmd_vel` subscription as an Arduino sketch, using the runtime API: topics are objects declared in the sketch, so there is no `r2s.yaml` and no generated code. The example sketch is `examples/arduino_minimal`, a node called `base_controller` with a one-line loop:

```text
r2s::Node node("base_controller");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
void loop()  { battery.data = read_battery(); battery_pub.publish(battery); node.spin_some(); }
```

Task-shaped sketches with the same API live beside it: `arduino_hello` (`chatter` publisher), `arduino_blink` (`led` subscription driving the LED), `arduino_button` (debounced `button` publisher), `arduino_servo` (`servo` subscription setting the angle), `arduino_range` (`range` publisher), and `arduino_action` (action server running one goal). Most headers name the host command to try the sketch with, and each builds for Uno
and Mega in CI.

You need `arduino-cli` with the AVR core and `avr-gcc`, plus libsimavr for the emulated run. One helper stages the Arduino library (the generator bundle, the core, the port glue and the runtime layer) and compiles the sketch, installing the AVR core when it is missing:

```sh
git clone https://github.com/apojomovsky/ros2serial.git
cd ros2serial
pip install ./gen
bash ports/arduino/build.sh --board uno
```

Build the simavr harness and start the firmware on it. It prints the pty the emulated UART is bridged to:

```sh
R2S_BUILD_DIR=$PWD/build bash tests/harness/simavr/run.sh --build-only
build/harness/simavr/r2s_simavr --mcu atmega328p --firmware build/uno/arduino_minimal.ino.elf --run-ms 300000
```

```text
r2s_simavr: pty=/dev/pts/0
```

In a second shell, build the bridge and launch it on that pty:

```sh
source /opt/ros/jazzy/setup.bash
colcon build --base-paths bridge --build-base build/bridge/build --install-base build/bridge/install
source build/bridge/install/setup.bash
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/pts/0 &
ros2 topic list
```

```text
/battery
/cmd_vel
/diagnostics
/parameter_events
/rosout
```

One reading proves the whole path:

```sh
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

```text
data: 0.0
```

The example publishes a fixed 0.0 reading, so that number proves the sketch, the emulator and the bridge agree.

## Moving to hardware

Not yet verified on hardware ([checklist](https://github.com/apojomovsky/ros2serial/issues/95)). Flash the same build to an Uno and point the bridge at the serial port instead of the pty:

```console
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno --input-dir build/uno examples/arduino_minimal
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

The Uno resets when its serial port opens, then runs its bootloader before your sketch starts. The bridge waits this out: `boot_wait_ms` in `bridge/r2s_bridge/config/r2s_bridge.params.yaml` defaults to 2500 ms, which covers the reset and the bootloader. If your board enumerates differently, substitute its port (often `/dev/ttyUSB0`) for `/dev/ttyACM0` in both commands.

## Virtual MCU

No board and no emulator to hand: the posix port runs the unmodified core as a host process on a virtual serial line. You need ROS 2 Jazzy with `colcon`, Python 3.10 or newer with `pip`, and a host compiler (`cmake`, `gcc`).

Install the generator and build the virtual firmware:

```sh
git clone https://github.com/apojomovsky/ros2serial.git
cd ros2serial
pip install ./gen
export R2S_BUILD_DIR=$PWD/build
bash ports/posix/run.sh --build-only
```

Tie two virtual serial ports together and start the firmware on one end. It announces a `battery` publisher and a `cmd_vel` subscription:

```sh
mkdir -p build/posix
python3 ports/posix/pty_pair.py > build/posix/ports.env &
sleep 1
set -a; source build/posix/ports.env; set +a
build/ports/posix/r2s_posix_firmware "$MCU_PORT" &
```

Build the bridge and launch it on the other end:

```sh
source /opt/ros/jazzy/setup.bash
colcon build --base-paths bridge --build-base build/bridge/build --install-base build/bridge/install
source build/bridge/install/setup.bash
ros2 launch r2s_bridge r2s_bridge.launch.py port:="$HOST_PORT" &
ros2 topic list
```

```text
/battery
/cmd_vel
/diagnostics
/parameter_events
/rosout
```

(Excerpt: the firmware also announces its test topics `/echo_vel`, `/joints`, `/log_text` and `/time_now`.) One reading proves the whole path:

```sh
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

```text
data: <counter>
```

Any number proves the path: the virtual MCU publishes its message counter as the reading.
