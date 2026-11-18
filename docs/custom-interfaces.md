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
The C++ runtime route reaches the same three kinds without the hooks, through
`r2s::Publisher`, `r2s::Service` and `r2s::action::Server` over a type library;
`tests/e2e/test_scenario_18_avr.py` runs that route against the same package.

The cancel hook is not optional: it is what answers the host. It calls
`r2s_actions_answer_cancel()` with `R2S_ACT_CANCEL_ACCEPTED`, which moves the
core's goal state to CANCELING, and the loop then ends the goal with a
`R2S_ACT_CANCELED` result through `r2s_actions_result()`, the way
`examples/custom_interfaces/custom_firmware/main.c` does. A hook left empty
leaves the host's cancel unanswered: the bridge reports the timeout to the ROS
client and the goal keeps running.

A service or action announce carries its type name, so its frame is longer than
a topic payload. Raise `limits.max_frame` when generation reports that the
configuration does not fit; the sample above uses 64 bytes for one of each kind.

### AVR and the generated service table

The generated service table carries `R2S_FLASH`, and `r2s_services_configure()`
reads every entry through `R2S_FLASH_READ_BYTE`, so it binds as emitted on AVR
and `examples/custom_interfaces/custom_firmware/main.c` passes it straight to
the bind. A table the firmware writes at run time lives in RAM and binds through
`r2s_services_configure_ram()`, the way the C++ registry does. That pair mirrors
topics and actions: `r2s_services_get()` returns an entry only for a RAM table,
and `r2s_services_copy()` reads a flash one out.

## Which route custom types take

A custom type needs descriptors, message headers and the type's hash, and only
`r2s-gen` emits them. Two routes carry them, and they differ in what the
firmware is built from rather than in what reaches the wire:

- **A type library for the X-macro and runtime routes.** `r2s-gen types` emits
  a library for just your types, so a sketch names one in an `R2S_TOPICS` row
  the way it names a bundled type, and a C++ sketch can register it as a
  runtime object instead. There is no `r2s.yaml` and no topic list.
  `examples/arduino_xmacro_custom` is the smallest complete firmware, built
  from the interface package beside it, and
  `tests/e2e/test_scenario_19_avr.py` runs it against the bridge. The
  runtime route's counterpart, `tests/e2e/avr_runtime_custom`, is scenario 18's.
- **The generated static API**, from `r2s.yaml` and `r2s-gen generate`: emit
  the pair with `--layout arduino` or `--layout cmake`, stage the core and
  your port glue beside it, and compile your sketch against that tree. It is
  what a bare-metal target uses, and what carries a service or an action
  through the generated indices.

`--layout arduino` writes `library.properties` plus the sources under `src/`
in both cases, which is a valid Arduino library directory, so a sketch builds
against either with the Arduino CLI and the flags that library's header
carries. Keep `r2s.yaml` when the firmware needs what a type list cannot
state: `limits.max_frame`, `features`, `tier`, and the service and action
indices the static C hook API switches on.

Both routes put the same bytes on the wire: the type's hash, its field layout
and its bounds come from the interface package either way, so the bridge
accepts a topic from either and cannot tell them apart.

Flash differs by the expansion itself, which defines the table, the publish
wrapper and the dispatch in the sketch's own translation unit instead of in
generated sources. Measured on one firmware body with one compiler and one flag
set, the X-macro build costs 74 bytes of flash and 1 byte of bss more than the
static one on an ATmega328P: 9856 text / 184 data / 157 bss against 9782 / 184
/ 156. The route is not the cost; the frame limit is. The bundled library sizes
its frame from a 31-character name allowance, the static route from the names
its configuration carries, so matching the limits is what makes the two builds
comparable.

Both firmware are budgeted beside the other examples in
[budgets](budgets.md).

## Bounds and unsupported input

A variable-length field gets its bound from its `.msg` definition or from `bounds`
in `r2s.yaml`, and from nothing else. With neither, generation fails naming the field:

```yaml
bounds:
  std_msgs/msg/Header.frame_id: 24
  sensor_msgs/msg/JointState.name: [4, 12]
  sensor_msgs/msg/JointState.position: 4
  sensor_msgs/msg/JointState.velocity: 4
  sensor_msgs/msg/JointState.effort: 4
```

Bound the field in the `.msg` when you can edit it (`string<=12 text`); `bounds`
is for definitions you cannot change. An entry on a field the `.msg` already
bounds is an error, in either direction, because a smaller MCU buffer cannot hold
what the host may legally send.

These bounds apply to the MCU storage; they do not alter the ROS interface.
Unsupported fields fail generation. Compact handshake and `--cpp` output are
not implemented: use the default full handshake and generated C API.
See the [generator specification](00-design.md#code-generator-specification)
for the schema and [troubleshooting](troubleshooting.md) for hash mismatches.

### The built-in types' limits

The bundled library's capacities are somebody's choice and may not be what a
built-in type's fields lead you to expect: a `std_msgs/msg/String` carries 16
characters, and `TFMessage` holds one transform. Check them before relying on
one:

```sh
ros2 run r2s_gen r2s-gen limits
```

Each row names the field, the storage it resolves to, and whether
`BUNDLE_BOUNDS` or the `.msg` supplied the capacity, so a limit the library
chose is told apart from one the definition states. A run prints one distro's
table, so ask for another with `--distro lyrical` or `--distro rolling`. To give
a built-in type a different limit, generate a library that carries it with your
bound, through the
[type library](#emit-a-type-library-for-the-x-macro-and-runtime-routes) route;
the library's own name differs, so Arduino cannot pick the stock one.

## Emit a type library for the X-macro and runtime routes

`r2s-gen types` emits an Arduino-style library for your own interface types, so
a custom type reaches the X-macro list and the runtime objects with the same
rows a bundled type uses. There is no `r2s.yaml` and no topic list: the input is
the types themselves.

```sh
ros2 run r2s_gen r2s-gen types my_robot_interfaces/msg/SensorReading \
  --msg-path ~/ros2_ws/src --layout arduino --out firmware/lib
```

A sketch then names the type like any bundled one:

<!-- snippet: skip until #648: names the reader's own package, which no staged fixture carries -->
```cpp
#define R2S_NODE_NAME "sensor_board"
#define R2S_TOPICS(X) \
  X(Publisher, my_robot_interfaces::msg::SensorReading, reading_pub, "reading")
#include <ros2serial.h>
```

Every variable-length field takes its bound from the `.msg` (`string<=12`,
`T[<=N]`) or from the command line, and a field with neither fails the command
naming the field:

```sh
ros2 run r2s_gen r2s-gen types my_robot_interfaces/msg/SensorReading \
  --msg-path ~/ros2_ws/src --out firmware/lib \
  --bound my_robot_interfaces/msg/SensorReading.text=24
```

`--bound` is repeatable and refuses a field the `.msg` already bounds, because a
smaller MCU buffer cannot hold a message the host may legally send; the message
names the field and the file that bounds it. A service or an action is named as
itself and expands to the parts its definition splits into, with the bound
naming the part, as `--bound my_robot_interfaces/action/Run_Result.note=24`.
A type list file replaces the command line when the list should be reviewed:
`--types-file r2s-types.yaml`, with one type per line and the same bounds.
`--write-config r2s-types.yaml` writes that file from a command you already ran.

The library's name carries the digest of its types and bounds, so two libraries
with different lists never collide and Arduino cannot pick the wrong one. The
sketch header includes every header the library ships, so a topic row names its
message type with no include of its own. The distro comes from `--distro`,
then `ROS_DISTRO`, then the default, and the warning about a sourced distro
that disagrees still applies. Regenerate the library after a `.msg` change: the
type hashes travel with it, and a stale copy makes the bridge reject every topic
at handshake rather than failing to compile.

The sketch header checks every row against the build's `R2S_MAX_FRAME` and
fails the build naming the object that does not fit, in both the C and the C++
spelling; the runtime route gets the same check as a `constexpr` over the
library. Each check counts the payload and the announce it makes, name length
varints included, so a limit one byte too small fails at that row rather than at
connect time. The frame limit itself stays a build flag, since it sizes the
core's own buffers: pass it to every translation unit, as the core's header
requires. A type list whose payloads need more than the protocol's 65277-byte
frame is refused when the library is generated.

A service or an action arrives through the same library, so a runtime sketch
names it like a message and writes no struct and no traits:

<!-- snippet: skip until #645: names the reader's own package, which no staged fixture carries -->
```cpp
#include <r2s/action.hpp>
#include <r2s/service.hpp>

r2s::Service<my_robot_interfaces::srv::Calibrate> calibrate_srv(node, "calibrate",
                                                               on_calibrate);
r2s::action::Server<my_robot_interfaces::action::Run> run("run", on_run_goal,
                                                          on_run_cancel,
                                                          on_run_accepted);
```

Your own action header carries the same two things a bundled one does: the
action struct with its `Goal`, `Result` and `Feedback` aliases, and the
`r2s::action::traits` specialization with the action's type name and hash, so
the server reads them from the library. A hand-written struct and trait are
what a sketch needed before the generator emitted them, and neither is needed
now.

Keep `r2s.yaml` when the firmware needs what a type list cannot state:
`limits.max_frame`, `features`, `tier`, and the generated service and action
indices that the static C hook API switches on.
