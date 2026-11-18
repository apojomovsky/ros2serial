# Building in C or without the Arduino library

Read this page if your firmware is compiled as C, or if you build with CMake,
PlatformIO or your own toolchain instead of the Arduino library. The runtime
objects are C++ and need the Arduino library, so on this side there are two
routes.

| Your build | Use |
| --- | --- |
| Arduino tools, sketch written in C | The X-macro list with C rows |
| CMake, PlatformIO, a bare-metal `main()`, a vendor toolchain | `r2s.yaml` and `r2s-gen` |
| Your own `.msg`, `.srv` or `.action` types, in C | `r2s.yaml`, or a type library from `r2s-gen types` |

Both routes are the same core: a C99 library with no heap, no function
pointers and no floating-point arithmetic, plus a port you provide.

## The X-macro list, in C

This is `examples/arduino_xmacro_c`: a `.c` file holding the list, next to a
small `.ino` that opens the port and forwards `setup()` and `loop()` to it.
Rows name the generated C struct types and callbacks take a pointer:

```c
#include "r2s/r2s.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
```

```c
#define R2S_NODE_NAME "base_controller"
#define R2S_TOPICS(X) \
  X(Publisher, std_msgs__msg__Float32, battery_pub, "battery") \
  X(Subscription, geometry_msgs__msg__Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>

void on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
}

void xmacro_setup(void)
{
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}
```

Your `loop()` calls `r2s_spin_some()` every pass, and `r2s_publish_battery_pub(&msg)`
once `r2s_connected()` returns nonzero.

C rows cost what C++ rows cost: `arduino_xmacro` and `arduino_xmacro_c` differ
by 2 bytes on an Uno (11364 against 11366, [size and stack
budgets](budgets.md)). The C rows also compile under the CH32V port's strict
flags, which the generator's own tests check.

This route still needs the Arduino library, because the rows expand inside a
sketch that the Arduino builder compiles. For any other toolchain, use the next
section.

## `r2s.yaml` and `r2s-gen`

Here the topics live in a file. The generator emits `r2s_generated.h` and
`r2s_generated.c`, and your firmware implements one handler per subscription,
named after the topic. This is `examples/pio_hello`, with the node renamed:

```yaml
node: base_controller
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
```

```c
#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
}

void setup(void)
{
    if (r2s_session_configure("base_controller", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}
```

Generate and build it:

```sh
ros2 run r2s_gen r2s-gen check --config r2s.yaml
ros2 run r2s_gen r2s-gen generate --config r2s.yaml --layout cmake --out generated
```

`examples/cmake_minimal` is the complete AVR version: the same code runs from
`main()`, and the firmware supplies the port functions the core calls:

- `r2s_port_read(buf, max)` fills a buffer from the link and returns how many
  bytes it took.
- `r2s_port_write(buf, len)` hands bytes to the link.
- `r2s_port_tx_free()` reports how many bytes the link accepts right now.
- `r2s_port_tick()` returns a free-running tick at `R2S_TICK_HZ` (1000 per
  second by default, so milliseconds) that wraps at 2^32.

The [porting guide](04-porting.md) states the contract each one owes.

## What each route costs and gives up

| | X-macro list, C rows | `r2s.yaml` and `r2s-gen` |
| --- | --- | --- |
| Language | C | C (C++ output is not emitted yet) |
| Toolchain | The Arduino library | Any: CMake, PlatformIO, Arduino layout, your own build |
| Topics | Rows inside the sketch | A file; the generator derives the table and the frame limit from it |
| Services | No row form | Generated indices, driven by the C service calls |
| Actions | `R2S_ACTIONS` rows from `<r2s/action.hpp>`, expanded once at file scope | `r2s_action_on_goal()` and `r2s_action_on_cancel()` hooks |
| Per-topic QoS | Always best effort | `qos:` per topic |
| Bounds | Bundled bounds, or a type library from `r2s-gen types` | `bounds:` in `r2s.yaml` |
| Frame limit | From the library header | Computed from the topics; `limits.max_frame` raises it |
| Extra build steps | None beyond the library | Run the generator, stage the core and your port glue |

On one firmware body, one compiler and one flag set, a prototype measured the
X-macro route and the `r2s.yaml` route at identical sizes on an ATmega328P:
7328 text, 184 data, 154 bss for both. The route costs nothing by itself; the
frame limit costs RAM, and the two routes compute it differently.

Moving from rows to `r2s.yaml` means rewriting the sketch against the generated
C API: handlers become `r2s_on_<topic>()`, publish calls become
`r2s_publish_<topic>()`, and the build moves from the Arduino library to the
generated pair plus the staged core and port. Check `R2S_MAX_FRAME` after the
switch.

## Custom types, on both routes

List your type under `topics:`, `services:` or `actions:` and run
`r2s-gen generate`. `examples/custom_interfaces` carries a custom message, a
service and an action this way, and `tests/e2e/test_scenario_17_avr.py` runs it
against the bridge.

In plain C with the X-macro route, `r2s-gen types` emits a library for your
types instead, so a row names your type as if it were bundled.
`examples/arduino_xmacro_custom` is the smallest complete firmware, and
`tests/e2e/test_scenario_19_avr.py` runs it against the bridge.

Bounds follow one rule everywhere: a variable-length field takes its bound from
its `.msg` (`string<=12`) or from your input, and a field with neither fails
generation, naming the field. [Message bounds](message-bounds.md) states the
rule, the conflict case and the built-in limits.

## Where to go next

- Add a topic to an Arduino sketch: [the firmware guide](firmware.md).
- Choose between the C++ routes: [choosing how to declare your
  topics](declaring-topics.md).
- Run the same firmware on your own board: [the porting guide](04-porting.md).
- Compare builds in bytes: [size and stack budgets](budgets.md).
