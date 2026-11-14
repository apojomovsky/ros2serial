# Custom messages and firmware generation

Use the bundled types for the shortest Arduino path. Use `r2s-gen` when you need
a custom `.msg`, different string/array bounds, or static C firmware.
The same ROS interface must be available on the host and to the generator.

## Define and build the ROS interface

Create a normal ROS interface package, for example `my_robot_interfaces`, with
`msg/SensorReading.msg`:

```text
float32 temperature
uint16 status
```

Its `package.xml` needs `ament_cmake` as a build tool,
`rosidl_default_generators` as a build dependency, `rosidl_default_runtime` as an
execution dependency, and membership in `rosidl_interface_packages`.
Its `CMakeLists.txt` uses the standard interface generation pattern:

```text
cmake_minimum_required(VERSION 3.22)
project(my_robot_interfaces)
find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
rosidl_generate_interfaces(${PROJECT_NAME} "msg/SensorReading.msg")
ament_export_dependencies(rosidl_default_runtime)
ament_package()
```

Build and source it before running the bridge or resolving installed interfaces:

```sh
cd ~/ros2_ws
colcon build --packages-up-to my_robot_interfaces
source install/setup.bash
ros2 interface show my_robot_interfaces/msg/SensorReading
```

The bridge loads host C++ type support dynamically. Installing just a `.msg`
file or just its Python class is insufficient.

## Generate explicitly with ros2 run

Create `r2s.yaml` in your firmware project:

```yaml
node: sensor_board
distro: jazzy
topics:
  - name: reading
    type: my_robot_interfaces/msg/SensorReading
    direction: pub
  - name: enabled
    type: std_msgs/msg/Bool
    direction: sub
```

```sh
ros2 run r2s_gen r2s-gen check --config r2s.yaml
ros2 run r2s_gen r2s-gen generate --config r2s.yaml --layout cmake --out generated
```

The generator searches explicit `--msg-path` directories first, then the sourced
workspace, then bundled definitions. A message search path contains package
directories, so `--msg-path ~/ros2_ws/src` can resolve
`~/ros2_ws/src/my_robot_interfaces/msg/SensorReading.msg` before it is installed.
Use the same distro for generation and the bridge.

The result contains `r2s_generated.h`, `r2s_generated.c`, and a CMake wrapper.
The header provides fixed-storage message types, typed publish functions such as
`r2s_publish_reading`, and declarations for handlers such as `r2s_on_enabled`.
Implement the subscription handler in your firmware. Do not edit generated files.

## Generate as part of colcon build

The `r2s_rosidl` package installs `r2s_generate_firmware()`, a CMake helper that
runs the same generator. It currently lives inside `gen/`; pass that nested
package explicitly so colcon discovers it alongside the enclosing package:

```sh
cd ~/ros2_ws
colcon build --base-paths src src/ros2serial/gen/r2s_rosidl \
  --packages-up-to r2s_rosidl --cmake-args -DBUILD_TESTING=OFF
source install/setup.bash
```

Create an `ament_cmake` package named `sensor_firmware` in `src/`, with your
`r2s.yaml`. Declare `ament_cmake` and `r2s_rosidl` as build tool dependencies and
`my_robot_interfaces` as a build dependency. Use this `CMakeLists.txt`:

```text
cmake_minimum_required(VERSION 3.22)
project(sensor_firmware)
find_package(ament_cmake REQUIRED)
find_package(r2s_rosidl REQUIRED)
r2s_generate_firmware(CONFIG r2s.yaml MSG_PATHS ..)
ament_package()
```

```sh
colcon build --base-paths src src/ros2serial/gen/r2s_rosidl \
  --packages-up-to sensor_firmware --cmake-args -DBUILD_TESTING=OFF
```

Sources appear under `build/sensor_firmware/r2s_generated/`.
`OUTPUT_DIR` changes that destination; `MSG_PATHS` accepts directories relative
to the firmware package. Invalid configurations fail the build with the
field or type that needs attention.

This step generates sources. It does not cross-compile or flash the MCU.
Compile them with your board toolchain and the matching ros2serial core and
port. The emitted CMake library accepts `R2S_CORE_DIR` pointing to the checkout's
`core/`; link it with your application and port implementation. The repository's
`examples/cmake_minimal` is a complete AVR C firmware showing that boundary.
The Arduino compile helper is for bundled runtime sketches, not this generated
static API. `--layout arduino` and `--layout platformio` emit alternative wrappers;
they also require the core and port to be staged by the firmware build.

## Services and actions

`r2s.yaml` takes a `services:` list and an `actions:` list beside `topics:`. A
service names its role, because the MCU can serve it, call it, or both:

```yaml
node: sensor_board
services:
  - name: calibrate
    type: my_robot_interfaces/srv/Calibrate
    role: server
  - name: snapshot
    type: my_robot_interfaces/srv/Snapshot
    role: client
actions:
  - name: run
    type: my_robot_interfaces/action/Run
```

The generator loads the `.srv` and `.action` definitions from the same search
path as messages, so the interface package that built them on the host is the
one your `--msg-path` reaches. The generated header carries one fixed-storage
struct per request, response, goal, feedback and result, plus an ID and a
descriptor index per entry:

```text
#define R2S_SERVICE_CALIBRATE_REQUEST_TYPE 1u
#define R2S_SERVICE_CALIBRATE_RESPONSE_TYPE 2u
#define R2S_ACTION_RUN_GOAL_TYPE 3u
#define R2S_ACTION_RUN_RESULT_TYPE 5u
```

Firmware answers a served request by taking it, decoding it, and sending the
response back under the `req_id` the take reported:

<!-- snippet: skip until #630: the example package's generated header is not a staged fixture -->
```c
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_services.h"
#include "r2s_generated.h"

void serve_calibrate(void)
{
    uint8_t index;
    uint8_t req_id;
    uint8_t payload[R2S_MAX_FRAME];
    uint16_t len = 0u;
    custom_interfaces__srv__Calibrate_Request request;
    custom_interfaces__srv__Calibrate_Response response;
    uint8_t field = 0xFFu;
    r2s_codec_out_t out = {payload, (uint16_t)sizeof payload, 0u, 0u};

    if (r2s_service_take_request(&index, &req_id, payload, (uint16_t)sizeof payload,
                                 &len) != R2S_OK) {
        return;
    }
    if (index != R2S_SERVICE_CALIBRATE_ID ||
        r2s_codec_decode(&r2s_generated_table, R2S_SERVICE_CALIBRATE_REQUEST_TYPE, payload,
                         len, 0u, &request, &field) != R2S_OK) {
        return;
    }
    response.accepted = 1u;
    /* fill response.note inside its bound */
    if (r2s_codec_encode(&r2s_generated_table, R2S_SERVICE_CALIBRATE_RESPONSE_TYPE,
                         &response, 0u, &out, &field) == R2S_OK) {
        (void)r2s_service_send_response(req_id, R2S_SVC_OK, payload, out.len);
    }
}
```

Actions are served through two hooks the core calls, `r2s_action_on_goal()` and
`r2s_action_on_cancel()`, and reported through `r2s_actions_feedback()` and
`r2s_actions_result()`. `examples/custom_interfaces/custom_firmware` implements
all three channels against one interface package and is the shortest complete
example, and `tests/e2e/test_scenario_17_avr.py` runs it against the bridge.

A service or action announce carries its type name, so its frame is longer than
a topic payload. Raise `limits.max_frame` when generation reports that the
configuration does not fit; the sample above uses 64 bytes for one of each kind.

### AVR and the generated service table

On AVR the generated service table is copied into RAM before
`r2s_services_configure()` sees it. The table lives in flash, and the core reads
it as ordinary memory, so it must be staged through `R2S_FLASH_READ_BYTE` first.
`examples/custom_interfaces/custom_firmware/main.c` keeps that copy in
`copy_services_to_ram()`. Topics and actions do not need this: the core reads
their tables through the flash reader already. The copy disappears once the
core learns the placement (#632).

## Which Arduino route custom types take

Custom types do not go through the Arduino sketches or the bundled runtime
layer. The bundled library ships the descriptors, message headers and topic
tables for its own types, and `r2s-arduino compile` stages exactly that bundle.
A custom type needs its own descriptors, which only `r2s-gen` emits, so the
generated static C API is the route: emit the pair with
`--layout arduino` or `--layout cmake`, stage the core and your port glue
beside it, and compile your sketch against that tree instead of the bundled
library. `--layout arduino` writes `library.properties` plus the pair under
`src/`, which is a valid Arduino library directory, so a sketch can be built
against it with the Arduino CLI and the same flags the bundle's build passes.

## Bounds and unsupported input

For unbounded strings and sequences, choose storage bounds explicitly:

```yaml
bounds:
  default_string_bound: 16
  std_msgs/msg/Header.frame_id: 24
  sensor_msgs/msg/JointState.name: [4, 12]
  sensor_msgs/msg/JointState.position: 4
  sensor_msgs/msg/JointState.velocity: 4
  sensor_msgs/msg/JointState.effort: 4
```

These bounds apply to the MCU storage; they do not alter the ROS interface.
Unsupported fields fail generation. Compact handshake and `--cpp` output are
not implemented: use the default full handshake and generated C API.
See the [generator specification](00-design.md#code-generator-specification)
for the schema and [troubleshooting](troubleshooting.md) for hash mismatches.
