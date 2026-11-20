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

The 0.5.0 board run passed on a Mega and was not run on an Uno, which was
unavailable; [real-board validation](hardware-validation.md#results) records
each run and what is still open. Flash the same build to your board and point
the bridge at the serial port instead of the pty:

```console
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno --input-dir build/uno examples/arduino_minimal
ros2 launch r2s_bridge r2s_bridge.launch.py port:=/dev/ttyACM0
ros2 topic echo /battery std_msgs/msg/Float32 --once
```

The Uno resets when its serial port opens, then runs its bootloader before your sketch starts. The bridge waits this out: `boot_wait_ms` in `bridge/r2s_bridge/config/r2s_bridge.params.yaml` defaults to 2500 ms, which covers the reset and the bootloader. If your board enumerates differently, substitute its port (often `/dev/ttyUSB0`) for `/dev/ttyACM0` in both commands.

### Checking a real board

The walkthrough above proves the path by hand, one command at a time.
`make hardware-check` does the same pass with one command and one verdict,
from the repository root, with the board plugged in and the dev image built
(`make image`):

```sh
make hardware-check PORT=/dev/ttyACM0 BOARD=uno
```

`BOARD` is `uno` or `mega`, and both arguments are required: the check
refuses to flash without them, so it cannot overwrite whatever a different
board is already running. The command flashes the `button_led` example,
starts the bridge, and:

- waits for the handshake, and prints how long it took from bridge start;
- samples `/button` for 30 s and requires 8 to 12 Hz with no silence over
  300 ms, so a board that publishes once and stalls fails;
- reads the link counters off `/diagnostics` and requires the MCU's CRC
  errors, receive overflows and dropped frames, and the bridge's per-topic
  sequence gaps and drops, at zero;
- publishes `/led` `true` and asks you to look at the board, then publishes
  `false`: nothing on the host can read an LED, so your answer is the check.

It ends with one block on stdout, the only thing to copy into a report; the
progress lines go to stderr, and the bridge's own output lands in
`hardware-check/bridge.log` under the build directory:

```text
== ros2serial hardware check
board: uno
port: /dev/ttyACM0
firmware: arduino_button_led arduino:avr:uno
flash: 11640 bytes
ram: 1181 bytes
handshake: 4.3 s
button rate: 10.0 Hz from 297 messages
longest gap: 120 ms
crc errors: 0
rx overflows: 0
dropped frames: 0
sequence gaps: 0
dropped topic frames: 0
led: lit
verdict: PASS
```

`verdict: PASS` exits 0; a failed run exits 1 and prints one `failed:` line
per reason above the verdict, including a link that never connected, for
example:

```text
failed: 450 ms with no /button message, over 300 ms
failed: the LED did not light when /led went true
verdict: FAIL
```

`handshake: never` with one `failed:` line is what a board running another
sketch, or a port a serial monitor still holds, reports: the firmware never
announces itself, so the counters, the rate and the LED all read `n/a`.

A run that cannot start at all exits 2 with what to do about it: no device at
the port, a board name that is not `uno` or `mega`, or a missing dev image.
The device reaches the container with its own group, so your user does not
need adding to `dialout`.

The checker itself is `tools/hardware_check.py`, and an emulated Uno runs it
in `tests/e2e/run-avr.sh`: `--skip-flash` stands in for the upload a
simulator has no place for, and `--sketch` builds a different sketch, which
is how a deliberately wrong run is proven to fail.

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
