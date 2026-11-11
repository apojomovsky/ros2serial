# Installation

[Start here](../README.md) for the complete board-to-topic walkthrough.
These instructions follow `master` and use a native Linux ROS workspace.
Docker is required for contributing to ros2serial, not for running it on a robot.

## Host packages

Use a supported ROS 2 installation: Jazzy, Lyrical, or Rolling. The walkthrough
uses Jazzy on Ubuntu 24.04. Install ROS using its
[installation instructions](https://docs.ros.org/en/jazzy/Installation.html),
including the development tools (colcon and rosdep). Initialize rosdep once
with `sudo rosdep init`, then run `rosdep update` as your user.

Clone the whole repository under your workspace's `src/`. The bridge builds
shared framing sources from `core/`, and the generator packages firmware assets
from the same checkout. Copying only `bridge/r2s_bridge` is insufficient.

```sh
source /opt/ros/jazzy/setup.bash
mkdir -p ~/ros2_ws/src
cd ~/ros2_ws/src
git clone https://github.com/apojomovsky/ros2serial.git
cd ~/ros2_ws
rosdep install --from-paths src --ignore-src -r -y
colcon build --packages-up-to r2s_bridge r2s_gen --cmake-args -DBUILD_TESTING=OFF
source install/setup.bash
ros2 pkg executables r2s_gen
ros2 run r2s_gen r2s-gen version
```

The generator supplies `r2s-gen` and `r2s-arduino`. The bridge executable is
`ros2 run r2s_bridge bridge`. No separate pip installation is needed.
The bridge also needs the ROS message packages your firmware uses, including
C++ introspection type support. For the first example this is `std_msgs`.

The repository is not yet distributed through the ROS apt repository
([#161](https://github.com/apojomovsky/ros2serial/issues/161)). Source installation
is the supported path here. Once binary packages are available, they can replace
this build step; the `ros2 run` and `ros2 launch` entry points remain the same.

## Arduino CLI

Install [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/)
and make `arduino-cli` available on your PATH. Install the AVR platform:

```sh
arduino-cli core update-index
arduino-cli core install arduino:avr@1.8.6
arduino-cli board list
```

The pinned AVR core matches the project's tested toolchain. Uno and Mega use
`arduino:avr:uno` and `arduino:avr:mega`. Other boards require their own platform;
compilation support is not a promise of hardware validation.

Prepare a library after sourcing your ROS workspace:

```sh
ros2 run r2s_gen r2s-arduino library --out ~/ros2_ws/firmware/ros2serial
```

The distro defaults to `ROS_DISTRO`; use `--distro jazzy` to choose explicitly.
Generation refuses an existing output directory, protecting edited sketches.
Keep your own sketch outside the generated library. To update, generate into a
new directory, compile against it, and flash again.

## Updating

Stop the bridge before flashing. Pull `master` in the source checkout, rerun
rosdep and colcon from a terminal with only the ROS underlay sourced, then source
the new workspace in a fresh terminal. Regenerate the firmware library and
recompile and upload your sketch. Mixing different protocol versions or message
definitions causes handshake rejection rather than compatible operation.

For reproducibility, record the checkout commit and Arduino platform version.
The command `ros2 run r2s_gen r2s-gen version` reports generator and wire versions.
A released checkout can be used instead, but follow the documentation shipped
with that release, which may have different tools.
