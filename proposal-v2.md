# rosserial for ROS 2: kickoff document (v2)

Oct 3, 2026 · @Alexis Pojomovsky · supersedes v1 (Oct 2, 2026)

> **How to use this document.** Part I is the design: what we build and why. Part II is the implementation specification: repository layout, exact protocol details, APIs, test strategy, CI, research tickets and the seed backlog. An agent creating tickets should start from Part II, section "Epics and seed tickets", and follow the links back into Part I for rationale. Where something is still unknown, Part II names a research ticket and a default to use until it is answered, so no ticket is blocked on a decision. Everything is built test-first, and every test runs on GitHub Actions.

# Part I: Design

## What changed since v1

| Topic | v1 | v2 |
| --- | --- | --- |
| Target quality | Implicit hobbyist scope | Hobbyist UX, professional robustness: written protocol spec, fuzzing, conformance tests |
| Hardware targets | ATmega328P, "AVR and PIC" | Three tiers (S, M, L); Uno and PIC18F4550 as tier M references; a PIC16 tier S |
| PIC toolchain | XC8 or SDCC, unverified | epic-cc and epic-hal as first-class targets, XC8 as secondary |
| Wire encoding | Native CDR (leaning) | Own packed little-endian layout; the bridge converts to CDR |
| `float64` on 8 bits | Open | Wire follows the `.msg`; opt-in "narrow" profile per topic (default where `double` is 32 bits) |
| Unbounded strings and sequences | Generator fails | Bounds come from the node config; the wire layout does not depend on them |
| Topic declaration | Runtime API | Declared in a node config file; the generator emits flash tables, typed publish functions and direct-call dispatch |
| Handshake | Two modes (negotiation and compiled table) | Full mode (names and types sent by the MCU); a compact hash-only mode for tier S |
| Schema check | Own hash | REP-2011 type hash (RIHS01), truncated |
| Minimum distro | Unspecified | Jazzy onwards |
| Code generator | rosidl plugin | Standalone generator reusing `rosidl_parser`; a rosidl plugin wraps it later |
| Receive path | Unspecified | Byte-feed API; optional ISR-safe ring for ports without one |
| Time | NTP-style on the MCU | rosserial-style `now()` on the MCU, with the estimation done on the host |
| User API | Unspecified | C99 first; C++ wrapper that compiles under strict Embedded C++ |
| Bridge language | C++ or Python | C++ (rclcpp), as a component |
| License | Apache 2.0 | MIT for the MCU core and generator; bridge Apache 2.0 or MIT |
| Plan | Seven spikes before code | Phase 0 research, then a vertical slice, then measuring and hardening |

## Vision and non-goals

The goal is to offer in ROS 2 what rosserial offered in ROS 1: publishing and subscribing to topics from a small microcontroller, down to 8-bit AVR and PIC, in a few steps and without having to learn DDS or micro-ROS.

The intended user experience is a hobbyist's: write a short config, include a header, publish, call spin. The intended engineering standard is a professional's: someone shipping a product on an 8-bit or small 32-bit MCU should be able to choose this library and defend that choice in a design review.

**Goals**

- A user experience equivalent to rosserial, with naming adapted to ROS 2 conventions.
- MCU core in C99, with no malloc, no external dependencies and no hardware access, portable across architectures (AVR, PIC, ARM Cortex-M, RISC-V), endianness and compilers (avr-gcc, epic-cc, XC8, arm-none-eabi-gcc).
- First-class support for 8-bit PIC through [epic-hal](https://github.com/apojomovsky/epic-hal) and [epic-cc](https://github.com/apojomovsky/epic-cc).
- On the host side, a regular ROS 2 node (Jazzy onwards) that works with any RMW (Fast DDS, Cyclone, rmw_zenoh).
- Transparency: the user writes or reuses `.msg` files and the MCU code is generated.
- A written, versioned protocol specification with test vectors, so third parties can implement it.

**Non-goals**

- Interoperability with MAVLink or other ecosystems.
- Competing with micro-ROS on 32-bit MCUs with an RTOS.
- Actions, full QoS, and taking part in DDS discovery from the MCU.
- Transports other than serial (UART, USB CDC) in the first version.
- ROS 1 or ROS 2 distros older than Jazzy.

## Hardware tiers

| Tier | Examples | RAM | Features |
| --- | --- | --- | --- |
| S | PIC16F193X (epic-hal), PIC16F877A (stretch) | 368 B to 1 KB | Publish and subscribe, compact handshake, narrow profile mandatory, optional log |
| M | Arduino Uno (ATmega328P), PIC18F4550 (epic-hal) | 2 KB | Everything in P0 and P1 |
| L | STM32, RP2040 | 32 KB or more | Everything; full-width `float64` by default |

Features are selected at compile time (`R2S_WITH_TIME`, `R2S_WITH_LOG`, `R2S_WITH_PARAMS`, `R2S_WITH_RELIABLE`), so a tier S build carries only publish and subscribe.

Proposed targets, to adjust with measurements:

| Tier | Core with 1 publisher + 1 subscriber (Float32, narrow Twist) |
| --- | --- |
| S | < 200 B RAM including buffers, < 4K words flash |
| M | < 400 B RAM including buffers, < 4 KB flash (P0 features) |

## Scope: parity with rosserial

| Feature | Priority | Notes |
| --- | --- | --- |
| Publish and subscribe | P0 | Float32 and Twist first, then Range, Imu and JointState |
| Handshake and topic announcement | P0 | Full mode; compact mode for tier S |
| Sync loss detection, reconnection, MCU reset detection | P0 | COBS resync, heartbeat, session ID |
| Logging to the host (`/rosout`) | P0 | Non-variadic, per-type API |
| Time synchronization (`now()` on the MCU) | P0 | Host-side estimation; see Time |
| Parameters | P1 | Declared by the MCU, exposed as ROS 2 parameters of the bridge node |
| Services (server and client) | P1 | Need the reliable channel |
| C++ wrapper | P1 | Strict Embedded C++ compatible; see User API |
| TF and diagnostics | P2 | Probably better handled on the host |
| Actions | Out of scope | |

The list is still based on prior knowledge of rosserial; the Phase 0 archaeology validates it against the source code.

## Architecture

```
 build time                                        runtime
 ----------                                        -------
 .msg files ──► r2s-gen (rosidl_parser) ──►  MCU firmware                          host
 r2s.yaml                   │                ┌──────────────────────┐   serial   ┌─────────────────────┐
 (node config)              ▼                │ user code            │  (COBS     │ r2s bridge (rclcpp) │
                 C99 headers and sources:    │  └ generated layer   │ ◄─frames─► │  ├ session          │
                 types, codecs, topic table, │     topic table,     │            │  ├ packed ⇄ CDR      │
                 typed publish functions,    │     codecs, dispatch │            │  │  (introspection)  │
                 dispatch; optional C++      │  └ r2s core (C99)    │            │  └ generic pub/sub  │──► any RMW
                 wrapper                     │  └ port glue         │            └─────────────────────┘
                                             └──────────────────────┘
```

The host has the message packages installed, as for any ROS 2 node. The MCU only carries the code for the types and topics declared in its config.

## Node configuration and generated code

Each firmware declares one node in a config file. The generator turns it into constant tables in flash and typed functions:

```yaml
# r2s.yaml
node: base_controller
handshake: full            # or: compact (tier S)
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
    qos: keep_latest
    narrow_float64: true
bounds:
  default_string_bound: 16   # optional; without it, unbounded fields are a generator error
  std_msgs/Header.frame_id: 24
  sensor_msgs/JointState.name: [4, 12]   # up to 4 names, 12 chars each
  sensor_msgs/JointState.position: 4
```

From this the generator emits:

- message structs and their codecs, for the types used only;
- the topic table (IDs, names, type names, hashes, flags) as constant data in flash;
- one typed publish function per published topic, for example `r2s_publish_battery(const std_msgs__msg__Float32 *)`;
- receive dispatch as a `switch (topic_id)` that calls a user-implemented handler for each subscription, for example `r2s_on_cmd_vel(const geometry_msgs__msg__Twist *)`;
- buffer sizes computed exactly from the largest message in each direction.

**Why generated tables and direct calls.** This keeps every call direct, with no function pointers. On PIC16 that is necessary: the hardware stack has 8 levels, and epic-cc lowers indirect calls into compare-and-call chains. It also keeps RAM use exact and puts strings in flash without passing string literals around at runtime. The same generated code is used on every target, so there is a single code path to test. The call depth from `r2s_spin_some` to a user handler is at most 3, and CI checks it.

Topic names still live in the firmware, in flash, and the launch file can still remap them (see User API).

## Wire encoding: packed, converted on the host

**What the choice is.** The wire encoding is the byte layout of a message on the serial line. DDS uses CDR. CDR aligns every field to its own size, inserting padding bytes, and prepends a 4-byte header. A packed layout writes fields one after the other with no padding. With CDR the bridge could forward bytes unchanged. With packed, the bridge converts.

**Decision: packed little-endian, converted to CDR by the bridge.**

- Lighter on the wire and in RAM: no padding, no encapsulation header.
- More portable: the generated code writes each field byte by byte in a fixed order, so it works the same on little- and big-endian MCUs, with or without alignment restrictions, and never `memcpy`s a struct.
- The conversion costs almost nothing on the host. The bridge walks the message with `rosidl_typesupport_introspection`, fills a message in memory and serializes it with `rmw_serialize` before publishing through a generic publisher. The bridge needs introspection anyway to handle types it was not compiled against.

The CDR comparison stays in the encoding benchmark (Phase 2) as a check on this decision, not as the deciding input.

**Encoding rules (draft for the spec)**

| `.msg` type | Wire |
| --- | --- |
| `bool`, `byte`, `char`, `int8`, `uint8` | 1 byte |
| `int16` to `int64`, `uint16` to `uint64` | Fixed width, little-endian |
| `float32` | IEEE 754 binary32, little-endian |
| `float64` | IEEE 754 binary64 by default; binary32 under the narrow profile |
| `string`, `string<=N` | Varint length + bytes, no terminator |
| `T[N]` (fixed array) | N elements, no prefix |
| `T[]`, `T[<=N]` | Varint length + elements |
| Nested message | Fields inline, in declaration order |

Lengths are varints (LEB128): 1 byte for values under 128, which covers almost all bounded fields. As a result **the layout does not depend on the bounds**. The host never needs to know the bound chosen for a field. It only needs to know whether the topic uses the narrow profile.

**`float64` and the narrow profile.** By default the wire carries what the `.msg` says. On compilers whose `double` is 32 bits (avr-gcc by default, epic-cc, some XC8 configurations), the generated code converts a C `float` to binary64 and back with byte-level bit manipulation. This is cheap, but it doubles the bytes. A topic can opt into the narrow profile (`narrow_float64: true`). All of its `float64` fields then travel as binary32 and the bridge widens them. On toolchains where `double` is 32 bits, the narrow profile is the default.

**No 64-bit integers in the core.** The core and the generated codecs never use 64-bit C types, because 8-bit toolchains handle them badly (epic-cc does not fully support `i64` yet). The binary64 conversion works on bytes. On toolchains without a usable 64-bit integer, `int64` and `uint64` fields are generated as an 8-byte little-endian array with helper functions.

**Bound violations** are never silently truncated. When publishing, the serializer returns an error. On reception, the decoder drops the frame and reports it to the host, which logs a warning naming the topic and field.

## Protocol draft (v1)

```
frame = COBS( topic_id[1] | seq[1] | payload[N] | crc16[2] ) + 0x00
```

| Field | Size | Purpose |
| --- | --- | --- |
| topic_id | 1 byte | 0 to 15 reserved for the system; 16 to 255 for user topics |
| seq | 1 byte | Per-direction, per-topic counter to detect lost frames |
| payload | N bytes | Packed message |
| crc16 | 2 bytes | Covers topic_id, seq and payload. Tentatively CRC-16/CCITT-FALSE (poly 0x1021), computed bitwise (no 512-byte table); confirm its Hamming distance at our maximum frame size against Koopman's tables |
| COBS and delimiter | 2 bytes up to 254 bytes of payload | One encoding byte (plus one per extra 254 bytes) and the trailing 0x00 |

**Reserved IDs (draft):** 0 handshake and control, 1 time, 2 log, 3 parameters, 4 heartbeat, 5 error report, 6 reliable-channel ack, 7 to 15 reserved.

**Handshake.** Either side can start it. The MCU sends `HELLO` on boot, and the host sends `HELLO_REQ` when it connects or detects a problem. The MCU's reply contains a header and one entry per topic.

- Header: protocol version, handshake mode, maximum frame size, RX capacity, tick rate, node name, and a random session ID chosen at boot.
- Topic entry, full mode: ID, direction (pub or sub), type name (for example `geometry_msgs/msg/Twist`), the first 8 bytes of the REP-2011 RIHS01 type hash, profile flags (narrow), topic name, and a QoS hint (best effort or reliable, keep-latest).
- Topic entry, compact mode (tier S): ID, direction, type hash, a hash of the topic name, and flags. The generator writes a matching YAML next to the firmware, and the bridge loads it (`node_map:=...`) to recover names and types. The names still come from the firmware's config; they just do not occupy its flash.

The bridge loads the type support for each type name and compares the hash with the one from the installed package. If they differ, it refuses that topic with a clear error, without dropping the session.

**Session and reconnection.** Heartbeats run in both directions. If the host sees a different session ID, the MCU has reset: the host redoes the handshake and the time sync. If the MCU stops seeing heartbeats, it stops publishing and waits for `HELLO_REQ`.

**Losses and errors.** User topics are best effort. A gap in `seq` increments a counter that the bridge publishes as diagnostics. Parameters, services and the handshake use a small reliable channel (stop-and-wait with ack on ID 6, one outstanding message per direction).

**Flow control.** The MCU announces its RX capacity. The bridge rate-limits host-to-MCU traffic per topic and, for keep-latest topics such as `cmd_vel`, keeps only the most recent message instead of queueing.

## Receiving bytes

The core never registers an interrupt or touches a UART. It exposes:

```c
void r2s_rx_feed(const uint8_t *buf, uint16_t len); /* main loop: hand over received bytes */
void r2s_rx_push(uint8_t byte);                      /* ISR-safe; only with R2S_WITH_RX_RING */
void r2s_spin_some(void);                            /* decode, dispatch, send heartbeats */
```

Many HALs already keep their own interrupt-driven ring (epic-serial, Arduino's `HardwareSerial`). On those, the port glue reads from it and calls `r2s_rx_feed`, and the core keeps no second ring, which would waste RAM on a tier S part.

For ports without a ring, `R2S_WITH_RX_RING` enables a single-producer, single-consumer ring fed by `r2s_rx_push` from the UART interrupt. It is safe on any target because:

- The ring is at most 256 bytes, so head and tail are `uint8_t`. A one-byte store is atomic on every MCU we target, so no locks or disabling interrupts are needed.
- Only the ISR writes `head` and only `spin_some` writes `tail`. Both are `volatile`.
- An optional `R2S_MEMORY_BARRIER()` hook (empty by default) covers cores that reorder memory accesses.

COBS decoding happens in place in the frame buffer, and the CRC is computed during decoding. On transmit, the payload is serialized straight into the TX buffer and the CRC is computed as it is written. Neither direction needs a second copy.

| Port | RX path |
| --- | --- |
| epic-hal (PIC16, PIC18) | `spin_some` drains `epic_serial_read` into `r2s_rx_feed` |
| Arduino (any core) | `spin_some` drains `Serial` into `r2s_rx_feed`. Works with the stock 64-byte buffer if `loop()` calls spin often enough |
| AVR bare metal | `R2S_WITH_RX_RING`: `ISR(USART_RX_vect) { r2s_rx_push(UDR0); }` |
| PIC with XC8, no HAL | Same as bare-metal AVR, in the UART RX interrupt |
| STM32, RP2040 | DMA in circular mode plus an idle-line interrupt, fed in bursts |

Transmission goes through a single port function, `r2s_port_write(buf, len)`.

## Time

**What rosserial did.** Right after connecting, and then every few seconds, the MCU asked the host for the current time. When the reply came, the MCU stored an offset from its own `millis()`, correcting for the round trip. From then on `nh.now()` was that offset plus `millis()`, with millisecond resolution. That is how sketches stamped their messages.

**Why it matters for IMUs.** A 150-byte Imu frame takes about 13 ms to send at 115200 baud. USB-serial adapters add jitter on top: an FTDI adapter's default latency timer is 16 ms. If the bridge stamped messages on arrival, the stamps would be late and irregular. Stamping at the moment of sampling needs `now()` on the MCU.

**Decision: keep rosserial's API, move the hard math to the host.**

1. The bridge periodically sends a ping on ID 1 and the MCU replies with its tick counter. Replying is the MCU's only job here.
2. The bridge estimates offset and drift, keeping the samples with the smallest round trip (they have the least delay).
3. The bridge sends the MCU a correction: "tick T0 corresponds to ROS time S0, with drift D ppm".
4. On the MCU, `r2s_now()` computes `S0 + (tick - T0)` with drift correction, in 32-bit integer arithmetic. Tick wraparound is handled with unsigned subtraction and by resyncing well before the counter wraps.

The tick comes from the port: `micros()` on Arduino, `epic_tick_get()` (1 ms) on epic-hal, a hardware timer elsewhere. The tick rate is announced in the handshake. The bridge also sets the serial port to low latency where the OS supports it. Target accuracy: about 1 ms, bounded by the tick resolution. Phase 2 measures the real figure.

## User API

### C (P0)

One node per firmware, so the API needs no node handle. Names follow rosserial where possible and ROS 2 conventions where they differ (`spin_some`, `msg` in type names). Generated message types reuse the rosidl C naming (`geometry_msgs__msg__Twist`), so ROS 2 users recognize them. They are not ABI-compatible with `rosidl_generator_c`, but the two never get linked together.

```c
#include "r2s_generated.h"        /* from r2s-gen, for the r2s.yaml above */

static std_msgs__msg__Float32 battery;

/* Declared by r2s_generated.h; called directly by r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    set_motors(msg->linear.x, msg->angular.z);
}

int main(void)
{
    r2s_init();
    for (;;) {
        battery.data = read_battery();
        r2s_publish_battery(&battery);
        r2s_spin_some();
    }
}
```

Logging is non-variadic, one function per value type, as in epic-serial's `put_*` API: `r2s_log_info("battery low")`, `r2s_log_info_u16("adc=", v)`. Stackless toolchains such as epic-cc cannot read varargs, and a format string costs flash on every target.

### C++ wrapper (P1)

**Compatibility bar: strict Embedded C++.** The wrapper compiles under the 1998 Embedded C++ subset: no templates, no namespaces, no exceptions, no RTTI, no multiple or virtual inheritance. It also avoids virtual functions, the standard library and placement `new`. That makes it build with avr-g++ (which has no standard library either), with epic-cc's C++ front end, and with any EC++ or Extended EC++ compiler.

The generator does what templates would otherwise do. It emits one concrete class per topic, sized from the config, with the port selected at compile time:

```cpp
#include "r2s_generated.hpp"

R2sNode node;                          // generated; sizes come from r2s.yaml
R2sPublisher_battery battery_pub(node);
std_msgs__msg__Float32 battery;

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg) { set_motors(msg->linear.x, msg->angular.z); }

void setup() { node.init(); }
void loop()  { battery.data = read_battery(); battery_pub.publish(battery); node.spin_some(); }
```

The objects are globals, as in rosserial sketches, which relies on static constructors running before `setup()`. A conformance test checks construction order across source files on every supported compiler.

An optional `r2s_namespaces.hpp` adds ROS 2-style names (`ros2serial::Node`, `std_msgs::msg::Float32`) as aliases, for compilers that support namespaces (avr-g++, epic-cc).

### Remapping from a launch file

The bridge creates its publishers and subscriptions under the names the MCU announces. ROS 2 applies remapping and namespace rules when a node creates a topic, so the standard launch syntax works with no extra code:

```python
Node(package='r2s_bridge', executable='bridge', namespace='robot1',
     parameters=[{'port': '/dev/ttyACM0', 'baud': 115200}],
     remappings=[('battery', 'power/battery')])
```

## Code generator and packaging

`r2s-gen` is a standalone Python tool (pip-installable, with no ROS installation required) that reuses `rosidl_parser`. Its inputs are `.msg` files and `r2s.yaml`. Its outputs are C99 headers and sources, the optional C++ wrapper, and the compact-mode node map. They are laid out to be usable as:

- an Arduino library (zip or Library Manager), like rosserial's `ros_lib`;
- a PlatformIO library;
- an epic-hal module (`depends_on` serial and tick), so `install.sh 18F4550 --modules serial,tick,ros2serial` scaffolds a ROS 2 node;
- a CMake or Makefile include for bare-metal and vendor toolchains.

A thin rosidl plugin wraps it later for users who want generation inside `colcon build`. The bridge, the generator and a bundle of the standard messages are released as ROS packages through the buildfarm.

## Host bridge

- C++ (rclcpp), packaged as a component so several bridges (one per serial port) can share a process.
- Generic publishers and subscriptions, plus introspection-based conversion from packed to CDR.
- Serial I/O: evaluate `transport_drivers`' `serial_driver` against plain asio.
- MCU log frames are forwarded to the bridge's logger under a child name (`<node>.mcu`), so they appear in `/rosout`.
- Compact mode: loads the node map generated with the firmware.
- P1 parameters: the MCU declares name, type and default in the handshake. The bridge declares them as ROS parameters on its own node, so `ros2 param set` works, and pushes changes to the MCU over the reliable channel.
- P1 services: the generic service APIs available in Jazzy still need checking. If they are missing, fall back to introspection-based typed services.

## epic-hal and epic-cc

8-bit PIC support goes through epic-hal (HAL and module packaging) and epic-cc (open-source C compiler, with C++ in progress). XC8 remains supported as a secondary toolchain. Using the same author's toolchain means compiler gaps found by ros2serial can be fixed at the source, and ros2serial gives epic-cc a realistic, protocol-heavy test program.

Constraints that shape the core. Most of them also make it leaner elsewhere.

| Constraint | Consequence for ros2serial |
| --- | --- |
| PIC16 hardware stack of 8 levels; a tick ISR uses 2 | Direct calls only, depth from `spin_some` to a handler of at most 3, checked in CI |
| Indirect calls lower to compare-and-call chains | No function pointers in the core or the generated code |
| No recursion (compile error) | Nested messages are flattened by the generator |
| `double` is 32 bits | Narrow profile by default |
| `i64` not fully supported yet | No 64-bit C types in the core or the codecs |
| No varargs, no stdio | Per-type logging API |
| Constant data lives in flash (RETLW tables, TBLRD) | Topic table and strings are generated as constant data |
| Classic PIC16 RAM is banked | Tier S prefers PIC16F193X (linear addressing); buffers are small and sized exactly |

**Testing.** The conformance vectors run on epic-cc's simulator and on epic-hal's simulator harness, next to simavr for AVR. Hardware in the loop on a PIC18F4550 board covers what the simulators do not.

## Quality bar

What "professional" means in concrete terms:

- **Specification:** a protocol document versioned independently of the code, with byte-level test vectors for every frame type.
- **Conformance suite:** the same vectors run against the C core (host build, simavr, epic-cc simulator) and against the bridge.
- **Fuzzing:** the COBS decoder, the CRC and every generated deserializer are fuzzed with libFuzzer on a host build. No input may crash, hang or write out of bounds.
- **C hygiene:** `-std=c99 -pedantic -Wall -Wextra -Werror`, cppcheck and clang-tidy in CI, no undefined behavior (UBSan on host builds). The code follows a MISRA-inspired subset; certification is not a goal.
- **Determinism:** no malloc, no recursion, no function pointers, bounded loops, a call depth checked in CI. Flash and RAM usage per configuration and per toolchain is reported in CI and tracked over time.
- **C++ wrapper:** compiled in CI with avr-g++ and epic-cc C++, under the strict Embedded C++ rules.
- **Versioning:** semver for the libraries and a protocol version checked in the handshake.
- **Hardware in the loop:** for the certified boards, starting with the Uno and a PIC18F4550.

## 8-bit gotchas

The tier M budget is 2 KB of RAM (ATmega328P, PIC18F4550); tier S goes down to a few hundred bytes.

- **`double` is 32 bits** in default avr-gcc and in epic-cc. Handled by the narrow profile or the binary64 conversion helpers.
- **64-bit integers** are slow on 8-bit cores and incomplete on some toolchains. Kept out of the core; byte-array fallback for message fields.
- **Imu is the stress case:** about 150 bytes under the narrow profile and about 300 at full width. A full-width Imu does not fit comfortably in a 2 KB RAM budget with RX and TX buffers. Use it to size the defaults and document the trade-off.
- **Arduino's stock RX buffer is 64 bytes.** Either call spin often, or use the bare-metal ISR port.
- **USB-serial latency:** the FTDI latency timer and USB frame scheduling affect time sync, not only throughput.
- **PIC16 call stack and banked RAM:** see the epic-hal and epic-cc section.
- **USB CDC on the PIC18F4550** costs a few KB of flash and some RAM for the USB stack. The hardware UART with an external USB adapter is much cheaper.

## Plan

Each phase ends in a decision gate and a publishable artifact.

**Phase 0: research (in parallel)**
- *rosserial archaeology:* feature catalog, RAM cost per publisher and subscriber, how time and reconnection really worked, known failure modes. Validates the parity table.
- *Prior art sweep:* ROS Discourse, GitHub, ROBOTIS ros2arduino, community rosserial-like ports. Explicit conclusion on whether to proceed.
- *Gate:* go or no-go. *Artifact:* a "state of serial MCUs in ROS 2" post.

**Phase 1: vertical slice**
- C core: COBS, CRC, minimal session, handwritten codecs for Float32 and Twist.
- `r2s-gen` producing those same codecs plus the topic table and dispatch, compared byte for byte against the handwritten ones.
- Bridge: handshake (full mode), generic pub/sub, introspection conversion.
- CI: host build plus simavr running the conformance vectors.
- *Demo:* teleop `cmd_vel` to a real Uno and a Float32 back, on Fast DDS and on rmw_zenoh.
- *Gate:* end-to-end works. *Artifact:* demo video and repository made public.

**Phase 2: measure and harden**
- Framing and CRC benchmark on AVR and on PIC18 (epic-cc): COBS vs SLIP vs rosserial style; CRC variants. Flash, RAM, cycles per byte, recovery time.
- Encoding benchmark: packed vs CDR vs MAVLink as a baseline, for Float32, Twist, Range, Imu, JointState.
- P0 complete: logging, time sync with measured accuracy, reconnection and reset detection, Range, Imu, JointState with bounds.
- Fuzzing, the protocol spec v1.0, and flash and RAM tracking.
- *Gate:* P0 complete and tier M budgets met. *Artifact:* benchmark write-up and the spec.

**Phase 3: PIC, portability and P1**
- PIC18F4550 on epic-hal and epic-cc (tier M), with XC8 as a secondary build.
- Tier S: compact handshake on a PIC16F193X part; the 877A as a stretch goal.
- A 32-bit target (STM32 or RP2040) for tier L.
- Parameters, services, the C++ wrapper (strict EC++), Arduino, PlatformIO and epic-hal packaging, rosidl plugin wrapper.
- *Demo:* the same sketch built for an Uno and for a PIC18F4550, both publishing into the same ROS 2 graph.
- *Gate:* release candidate. *Artifact:* a "port your rosserial sketch" guide and the cross-compiler demo.

**Phase 4: release and community**
- Buildfarm release of the bridge and generator, docs site, Discourse announcement.
- Possible move into a community organization (ros-drivers or similar).

**Role of the agents.** Each spike or workstream gets its own agent. A shared harness (simavr, epic-cc simulator, host builds, conformance vectors, flash and RAM reports) makes the agents produce comparable tables. A person reads those tables and makes the design decisions.

**Other proposed targets (to adjust with Phase 1 data)**

| Metric | Target |
| --- | --- |
| Framing + CRC on AVR | < 1 KB flash, < 50 B RAM (excluding buffers) |
| Recovery after corruption | Next frame after the next 0x00 |
| Time sync accuracy | about 1 ms at 115200 baud, bounded by the tick resolution |
| Call depth from `spin_some` to a user handler | ≤ 3 |

## Open decisions and risks

Each open decision has a default. Tickets use the default until the decision is made; changing a default later is its own ticket.

| Decision | Default until decided | Decided by |
| --- | --- | --- |
| Project name and C prefix | Repository `ros2serial`, C prefix `r2s_`, ROS packages `r2s_*` | Maintainer, before Phase 4 |
| Topics only from the config file, or also a runtime registration API on tier L | Config file only | Maintainer, after Phase 1 feedback |
| Message type names (`__` is reserved in C++) | `std_msgs__msg__Float32`, as rosidl does | Maintainer, before the C++ wrapper (Phase 3) |
| CRC variant | CRC-16/CCITT-FALSE | Research ticket R7 |
| Type hash truncation on the wire | 8 bytes | Spec ticket S2 |
| Narrow profile per field or per topic | Per topic | Maintainer, after the Phase 2 encoding benchmark |
| Serial library for the bridge | Plain POSIX `termios` behind a small interface | Research ticket R8 |
| Generic service support in Jazzy | Assume missing; use introspection-based typed services | Research ticket R4 |
| Bridge license | MIT, like the MCU side | Maintainer, before the first release |
| Home | Personal repository; community organization at Phase 4 | Maintainer |

| Risk | Mitigation |
| --- | --- |
| Scope grows from trying to be universal | Tiers with explicit feature sets; Uno first, PIC in Phase 3 |
| Packed layout and bridge conversion drift apart | Single written spec; the conformance vectors run on both sides in CI |
| Introspection conversion is slow for large messages | Measure in Phase 2; cache per-type conversion plans |
| Config file feels like friction compared with rosserial | Ship bound defaults for common messages; keep the config to a few lines; consider a runtime API on tier L |
| epic-cc gaps block the PIC targets | Same maintainer; file gaps with minimal repros from ros2serial; XC8 as fallback |
| Prior art exists that was not found | Phase 0 sweep before investing in code |
| Low demand as 32-bit MCUs get cheaper | Validate early; the value is the experience and robustness, not only size, and the core also runs on 32-bit MCUs |

---

# Part II: Implementation specification

## Repository layout

One repository, one colcon workspace root. Every directory has its own tests next to its code.

```
ros2serial/
├── AGENTS.md                 conventions for agents and contributors (summarizes this Part II)
├── LICENSE                   MIT
├── spec/
│   ├── protocol.md           the normative protocol specification (grows from Part I + this Part II)
│   └── vectors/              conformance vectors (YAML), the single source of truth for every codec test
│       ├── crc/  cobs/  varint/  frames/  messages/  session/
├── core/                     C99 MCU core (no ROS, no hardware)
│   ├── include/r2s/          public headers: r2s.h, r2s_config.h, r2s_port.h
│   ├── src/                  crc.c, cobs.c, framer.c, session.c, time.c, log.c, ring.c
│   ├── tests/                Unity unit tests, vector runner, CMakeLists.txt
│   └── fuzz/                 libFuzzer targets and seed corpora
├── gen/                      r2s-gen: Python package (pip) and ament_python package (ROS)
│   ├── r2s_gen/              cli.py, config.py, msgs.py, hashing.py, reference/ (Python reference codec), templates/
│   └── tests/                pytest: unit, golden-file, property-based
├── bridge/
│   ├── r2s_bridge/           C++ rclcpp component + executable, launch/, config/
│   └── r2s_bridge_tests/     launch_testing integration tests (ROS package, test-only)
├── ports/
│   ├── posix/                host "virtual MCU" over a pty: used by integration tests and fuzzing
│   ├── arduino/              Arduino library glue (Serial, micros)
│   ├── avr/                  bare-metal AVR glue (USART ISR, timer tick)
│   ├── epic-hal/             epic-serial + epic-tick glue, epic-hal module manifest entry
│   └── stm32/                Phase 3
├── examples/                 one directory per example, each built in CI
├── tests/
│   ├── e2e/                  shared end-to-end scenarios, run against posix, simavr and PIC simulators
│   └── harness/              simavr UART-to-pty harness, fault-injecting pty proxy
├── tools/                    size_report.py, call_depth.py, budgets.yaml, vectors_check.py
├── docs/                     user documentation (later the docs site)
└── .github/workflows/
```

**Code reuse between MCU and host.** The bridge links `core/src/crc.c`, `cobs.c` and `framer.c` compiled for the host. Framing and CRC therefore have a single implementation. The host session logic is separate C++.

## Engineering conventions

| Area | Convention |
| --- | --- |
| C | C99, `-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow`, `clang-format` (config in repo), `stdint.h` types only, no `int`/`long` in interfaces, no 64-bit types in core, no function pointers, no recursion, no malloc, no VLAs, no `printf` |
| C++ (bridge) | C++17 (Jazzy's baseline), `ament_lint_auto` (cpplint, uncrustify or clang-format, cppcheck), ROS 2 style |
| C++ (MCU wrapper) | Strict Embedded C++, see Part I |
| Python | 3.10 or newer, `ruff` (lint + format), `mypy --strict` on `r2s_gen`, `pytest`, `hypothesis` |
| Commits | Conventional Commits (`feat(core): ...`, `fix(bridge): ...`, `test(gen): ...`); changelog with git-cliff, as in epic-hal and epic-cc |
| Branches and PRs | `main` is protected; one ticket per PR; squash merge; CI must be green; the PR body links the ticket and pastes the red test run (see TDD rules) |
| Versions | semver per component (`core`, `gen`, `bridge`) plus the protocol version (`R2S_PROTOCOL_VERSION`, starts at 1) |
| Licenses | MIT for everything by default (see open decisions) |
| Docs | Every public function has a doc comment; every PR that changes behavior updates `spec/protocol.md` or `docs/` in the same PR |

## Protocol details

All multi-byte integers are little-endian. "string" means varint length followed by bytes, without a terminator. Sizes are in bytes.

### Frame and limits

- `frame = COBS(topic_id u8 | seq u8 | payload | crc16 u16) 0x00`, as in Part I.
- `seq` is one counter per `topic_id` per direction. It starts at 0 after each handshake and wraps at 255.
- The maximum unencoded frame (`topic_id` to `crc16`) is set at build time (`R2S_MAX_FRAME`, generated) and announced in `HELLO`. The protocol allows up to 65535. The host never sends a frame larger than the MCU announced.
- A frame shorter than 4 bytes after COBS decoding, with a bad CRC, or longer than `R2S_MAX_FRAME`, is dropped and counted.

### System channels

The first payload byte of IDs 0 and 1 is an opcode.

**ID 0, control**

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | HELLO_REQ | host → MCU | `protocol_version u8` |
| 0x02 | HELLO | MCU → host | `protocol_version u8, handshake_mode u8 (0 full, 1 compact), session_id u32, max_frame u16, rx_capacity u16, tick_hz u32, features u8, topic_count u8`, then `node_name string` (full) or `node_name_hash u32` (compact) |
| 0x03 | TOPIC_REQ | host → MCU | `index u8` (0 to topic_count − 1) |
| 0x04 | TOPIC | MCU → host | `index u8, topic_id u8, flags u8, type_hash[8]`, then `type_name string, topic_name string` (full) or `topic_name_hash u32` (compact) |
| 0x05 | TOPIC_STATUS | host → MCU | `topic_id u8, status u8 (0 ok, 1 unknown type, 2 hash mismatch, 3 name conflict)` |
| 0x06 | READY | host → MCU | none |

- `features` bits: 0 time, 1 log, 2 params, 3 reliable channel. The other bits are reserved and must be 0.
- `flags` bits: 0 direction (0 = the MCU publishes, 1 = the MCU subscribes), 1 narrow_float64, 2 keep_latest, 3 reliable. The other bits are reserved.
- Name hashes in compact mode are FNV-1a 32-bit over the UTF-8 name. Default; spec ticket S2 confirms it.
- Each topic is requested and sent one at a time, so a tier S MCU never needs a buffer larger than one TOPIC frame.

**ID 1, time**

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | PING | host → MCU | `ping_id u8` |
| 0x02 | PONG | MCU → host | `ping_id u8, tick u32` (tick sampled when the PING is dispatched) |
| 0x03 | CORRECTION | host → MCU | `tick0 u32, sec i32, nanosec u32, drift_ppb i32` |

**ID 2, log** (MCU → host): `level u8` (ROS severities: 10 debug, 20 info, 30 warn, 40 error, 50 fatal), `text string`, `value_kind u8` (0 none, 1 u16, 2 i16, 3 u32, 4 i32, 5 f32), then the value if present.

**ID 3, parameters**: P1, specified by ticket S5.

**ID 4, heartbeat**: host → MCU has an empty payload. MCU → host carries `rx_frames u16, crc_errors u16, rx_overflows u16, dropped_frames u16`, all wrapping counters.

**ID 5, error report** (MCU → host): `code u8, topic_id u8, count u16`. Codes: 1 bound violation on decode, 2 unknown topic ID, 3 RX overflow, 4 frame too large, 5 publish while not connected (counted, reported once per connection).

**ID 6, reliable-channel ack**: P1, specified by ticket S5.

### State machines

**MCU**

```
BOOT ──init──► ANNOUNCING ──READY──► CONNECTED
                  ▲   │                  │
                  │   └─ every 1 s with no host traffic: send HELLO
                  └──────── link timeout, or HELLO_REQ received ─┘
```

- ANNOUNCING: replies to HELLO_REQ with HELLO and to TOPIC_REQ with TOPIC; stores TOPIC_STATUS per topic; ignores user topic frames; `r2s_publish_*` returns `R2S_ERR_NOT_CONNECTED` and sends nothing.
- CONNECTED: sends a heartbeat every `R2S_HEARTBEAT_MS` (default 500); publishing works for topics with status ok.
- Link timeout: no valid frame from the host for `R2S_LINK_TIMEOUT_MS` (default 2000).
- `session_id` is chosen once per boot from `r2s_port_seed()`.

**Host**

```
DISCONNECTED ──port open──► HANDSHAKING ──all topics resolved, READY sent──► CONNECTED
     ▲                           ▲                                               │
     └── port error ─────────────┴──── link timeout or session_id change ────────┘
```

- DISCONNECTED: tries to open the port every `reconnect_period_ms` (default 1000).
- HANDSHAKING: sends HELLO_REQ every 500 ms until HELLO arrives. Rejects a protocol version it does not support, and logs an error naming both versions. Then, for each index, sends TOPIC_REQ with a 200 ms timeout and 3 retries. Resolves the type support, checks the hash, sends TOPIC_STATUS, creates or reuses the publisher or subscription, and finally sends READY.
- CONNECTED: heartbeat every 500 ms, time sync (see below), forwarding.
- On re-handshake, publishers and subscriptions are kept if the new topic table is identical to the previous one. Otherwise they are destroyed and recreated.

**Time sync on the host.** Right after READY, 8 PINGs 50 ms apart. Then one PING every `time_sync.period_ms` (default 1000 for the first 10 s, then 5000). The estimator keeps a sliding window of the 16 most recent samples, uses the lowest round trip ones for the offset, and a linear fit for drift. It sends a CORRECTION after the initial burst and whenever the estimate moves by more than 100 µs.

## C API reference

### Public headers

```c
/* r2s.h */
typedef int8_t r2s_status_t;          /* 0 ok, negative errors */
#define R2S_OK                    0
#define R2S_ERR_NOT_CONNECTED    (-1)
#define R2S_ERR_BOUNDS           (-2)
#define R2S_ERR_TX_BUSY          (-3)
#define R2S_ERR_TOPIC_REJECTED   (-4)

typedef struct { int32_t sec; uint32_t nanosec; } r2s_time_t;   /* builtin_interfaces/Time */

void r2s_init(void);
void r2s_spin_some(void);
void r2s_rx_feed(const uint8_t *buf, uint16_t len);
void r2s_rx_push(uint8_t byte);                         /* R2S_WITH_RX_RING only, ISR-safe */
uint8_t r2s_connected(void);
r2s_time_t r2s_now(void);                               /* R2S_WITH_TIME only; {0,0} before the first CORRECTION */
uint8_t r2s_time_synced(void);                          /* R2S_WITH_TIME only */

/* R2S_WITH_LOG only; one family per level: debug, info, warn, error, fatal */
void r2s_log_info(const char *text);
void r2s_log_info_u16(const char *text, uint16_t v);
void r2s_log_info_i16(const char *text, int16_t v);
void r2s_log_info_u32(const char *text, uint32_t v);
void r2s_log_info_i32(const char *text, int32_t v);
void r2s_log_info_f32(const char *text, float v);
```

### Port contract

Each port implements these as ordinary functions, resolved at link time:

```c
/* r2s_port.h */
void     r2s_port_write(const uint8_t *buf, uint16_t len);  /* may block; must send all bytes */
uint32_t r2s_port_tick(void);                               /* monotonic, wraps; rate = R2S_TICK_HZ */
uint32_t r2s_port_seed(void);                               /* entropy for session_id; differs across boots where possible */
```

A port that has its own receive buffer also reads from it in the user's loop and calls `r2s_rx_feed`. The Arduino and epic-hal ports provide an `r2s_port_poll()` helper that does this.

### Generated API

For each topic in `r2s.yaml`:

```c
r2s_status_t r2s_publish_<name>(const <Type> *msg);   /* published topics */
void r2s_on_<name>(const <Type> *msg);               /* subscribed topics: prototype generated, user implements */
```

`<name>` is the topic name with `/` replaced by `_`. A topic name that cannot be mapped to a valid C identifier is a generator error.

### C type mapping

| `.msg` | C |
| --- | --- |
| `bool` | `uint8_t` (0 or 1; any other value on the wire is a decode error) |
| `byte`, `char`, `uint8` | `uint8_t` |
| `int8` … `int32`, `uint16` … `uint32` | `int8_t` … `uint32_t` |
| `int64`, `uint64` | `int64_t`, `uint64_t`; with `R2S_NO_INT64`, `r2s_i64_t`/`r2s_u64_t` (8-byte arrays plus helpers) |
| `float32` | `float` |
| `float64` | `double` (32 bits on avr-gcc and epic-cc; documented precision loss) |
| `string<=N` | `char name[N + 1]`, NUL-terminated |
| `T[N]` | `T name[N]` |
| `T[<=N]` | `struct { uint8_t size; T data[N]; }` (`uint16_t size` if N > 255) |
| Nested message | Nested struct |

## Code generator specification

### Command line

```
r2s-gen generate --config r2s.yaml --out <dir> [--msg-path <dir>]... [--layout cmake|arduino|platformio|epic-hal] [--cpp]
r2s-gen check    --config r2s.yaml [--msg-path <dir>]...       # validate only, exit code 0 or 1
r2s-gen vectors  --type <pkg/msg/Type> --message <yaml>         # print payload hex from the reference codec
r2s-gen version
```

### Message sources

The generator searches `--msg-path` directories first, then `share/*/msg` under `AMENT_PREFIX_PATH` when ROS is sourced, and finally a bundled snapshot of `common_interfaces` and `rcl_interfaces` (the Jazzy release, Apache 2.0, with attribution) so it works without ROS.

### Config schema

`r2s.yaml` is validated against a JSON Schema shipped in the package (`r2s_gen/schema/r2s.schema.json`). Keys:

| Key | Type | Required | Default |
| --- | --- | --- | --- |
| `node` | string, valid ROS node name | yes | |
| `handshake` | `full` or `compact` | no | `full` |
| `tier` | `S`, `M` or `L` (selects defaults and budget checks) | no | `M` |
| `features` | list of `time`, `log`, `params`, `reliable`, `rx_ring` | no | `[time, log]` |
| `topics[].name` | relative ROS topic name | yes | |
| `topics[].type` | `pkg/msg/Type` | yes | |
| `topics[].direction` | `pub` or `sub` | yes | |
| `topics[].qos` | `best_effort`, `keep_latest`, `reliable` | no | `best_effort` |
| `topics[].narrow_float64` | bool | no | `true` if `tier` is S, else `false` |
| `bounds` | map of `pkg/Type.field` to an int, or `[count, string_bound]` | no | |
| `bounds.default_string_bound` | int | no | none (unbounded fields are errors) |
| `limits.max_frame` | int, overrides the computed value | no | computed |

Generator errors are precise and actionable: file, key or field path, what is wrong, and how to fix it. Each error message has a golden test.

### Outputs

- `r2s_generated.h` and `r2s_generated.c`: types, codecs, topic table, publish functions, dispatch, `R2S_MAX_FRAME`, feature macros, the protocol version.
- `r2s_generated.hpp` and `r2s_namespaces.hpp` with `--cpp`.
- `r2s_node_map.yaml` in compact mode.
- A `layout`-specific wrapper: `library.properties` (Arduino), `library.json` (PlatformIO), a module entry (epic-hal), `CMakeLists.txt` (cmake).
- Every output file starts with a header naming the generator version and config hash. Output is deterministic: same inputs, byte-identical files.

### Type hash

The generator computes the REP-2011 RIHS01 hash for each type. Default approach: reuse `rosidl_generator_type_description`'s algorithm (research ticket R3 decides whether it is imported or vendored). A test compares the generator's hash with the one in the installed Jazzy type support for every bundled type.

## Bridge specification

### Executable and component

- Package `r2s_bridge`, component `r2s_bridge::Bridge`, executable `bridge`.
- One instance per serial port.

### Parameters

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `port` | string | `/dev/ttyACM0` | Serial device |
| `baud` | int | 115200 | Baud rate |
| `low_latency` | bool | true | Request low-latency mode on the port when supported |
| `node_map` | string | empty | Path to `r2s_node_map.yaml` (compact mode) |
| `reconnect_period_ms` | int | 1000 | |
| `heartbeat_period_ms` | int | 500 | |
| `link_timeout_ms` | int | 2000 | |
| `time_sync.enabled` | bool | true | |
| `time_sync.period_ms` | int | 5000 | |
| `rate_limit_hz` | double | 0 (none) | Default host-to-MCU rate limit per topic |
| `rate_limit_hz.<topic>` | double | | Per-topic override |
| `qos_depth` | int | 10 | Depth for publishers and subscriptions created for the MCU |

### Published by the bridge

- `/diagnostics` (via `diagnostic_updater`): link state, session ID, protocol version, MCU counters from heartbeats, sequence gaps per topic, time sync offset and round trip.
- The MCU's topics, under the names it announced, after remapping and namespacing.
- MCU log messages through the bridge's logger (`<node_name>.mcu`), so they reach `/rosout`.

### Implementation pointers

- Generic publishers and subscriptions: `rclcpp::Node::create_generic_publisher` and `create_generic_subscription`.
- Type support at runtime: `rclcpp::get_typesupport_library` and `rclcpp::get_typesupport_handle` with `rosidl_typesupport_introspection_cpp`.
- Installed type hash: the `get_type_hash_func` member of the message type support.
- Packed to CDR: build the message in memory with the introspection `init_function`, fill it field by field, then `rmw_serialize`. CDR to packed for subscriptions: `rmw_deserialize`, then walk the fields. Conversion plans are built once per type and cached.

## Test strategy

### TDD rules

1. Every ticket starts with tests. The first commit in a PR adds tests that fail, and the PR body includes the failing run (command and output excerpt).
2. Behavior that crosses the wire is specified by a conformance vector in `spec/vectors/` before any code is written. Code in C, Python and C++ is tested against the same vectors.
3. Bugs are fixed by first adding a test (or a vector, or a fuzz corpus entry) that reproduces them.
4. No test is deleted or weakened to make CI pass without the maintainer's approval in the PR.
5. CI is the source of truth: a test that only runs locally does not count.

### Test oracles

| Oracle | Checks |
| --- | --- |
| Conformance vectors (`spec/vectors/`) | C core, Python reference codec, bridge framing and conversion |
| Python reference codec (`r2s_gen/reference/`) | A deliberately simple, readable implementation of the encoding rules. It generates vectors and is the oracle for property-based tests |
| `rclpy.serialization.serialize_message` / rclcpp serialization | The bridge's packed-to-CDR output, byte for byte |
| Installed type support hashes | The generator's RIHS01 hashes |
| Published check values | CRC-16/CCITT-FALSE of `"123456789"` is `0x29B1`; COBS examples from the original paper and Wikipedia |

### Vector format

```yaml
# spec/vectors/messages/float32_1_5.yaml
name: float32_1_5
type: std_msgs/msg/Float32
profile: {narrow_float64: false}
message: {data: 1.5}
payload: "00 00 c0 3f"
```

```yaml
# spec/vectors/cobs/basic.yaml
cases:
  - {decoded: "00",          encoded: "01 01 00"}
  - {decoded: "00 00",       encoded: "01 01 01 00"}
  - {decoded: "11 22 00 33", encoded: "03 11 22 02 33 00"}
  - {decoded: "11 22 33 44", encoded: "05 11 22 33 44 00"}
```

```yaml
# spec/vectors/varint/basic.yaml
cases:
  - {value: 0,   encoded: "00"}
  - {value: 127, encoded: "7f"}
  - {value: 128, encoded: "80 01"}
  - {value: 300, encoded: "ac 02"}
```

Frame and session vectors (full frames with CRC, handshake transcripts as ordered lists of host and MCU frames) are produced by the reference implementation and reviewed by hand once, then frozen. `tools/vectors_check.py` validates every vector file against a schema in CI.

### Test layers

| Layer | What | Tooling | Runs |
| --- | --- | --- | --- |
| Core unit | CRC, COBS, varint, framer, ring, session state machine, time math, log encoding | Unity (ThrowTheSwitch, MIT), CMake/CTest, gcc and clang, `-m32` build too | Every PR |
| Core vectors | Every vector in `spec/vectors/` through the C code | Unity runner generated from YAML | Every PR |
| Core sanitizers | Unit tests under ASan + UBSan | clang | Every PR |
| Core coverage | Line and branch coverage | gcovr; fail below 95% lines for `core/src` | Every PR |
| Big-endian | Core unit tests on a big-endian target | qemu-user (for example s390x) via docker | Nightly |
| Fuzzing | COBS decoder, framer, every generated deserializer, session input | libFuzzer; corpora committed in `core/fuzz/corpus/` | 60 s per target on PRs, 30 min nightly |
| Generator unit | Config validation, bounds resolution, naming, error messages | pytest | Every PR |
| Generator golden | Generated files for example configs match committed snapshots | pytest; `--update-golden` flag for intentional changes | Every PR |
| Generator property | Random message definitions and values: generate C, compile on host, round-trip, compare with the reference codec | hypothesis + host gcc | Every PR (bounded examples), more nightly |
| Generator hashes | RIHS01 equals the installed type support's hash for bundled types | pytest inside the ROS container | Every PR |
| Bridge unit | Host session state machine with a fake transport, converter against rclcpp serialization, time estimator with a simulated drifting clock, rate limiting | gtest via `ament_cmake_gtest` | Every PR |
| End-to-end, posix | Bridge plus the posix virtual MCU over a pty: handshake, pub, sub, log, time, remapping, reconnect | launch_testing | Every PR, RMW matrix |
| Fault injection | A pty proxy between bridge and MCU that flips bits, drops, duplicates and inserts bytes, truncates frames and resets the MCU mid-session | launch_testing + `tests/harness/` | Every PR (short), nightly (long) |
| End-to-end, AVR | The same scenarios against firmware running in simavr (ATmega328P), UART bridged to a pty | simavr harness | Every PR once ticket R5 lands |
| End-to-end, PIC | The same scenarios on epic-cc's simulator or epic-hal's host simulation | Decided by research ticket R6 | Every PR once available |
| Build matrix | Examples compile for Uno (arduino-cli), PlatformIO, bare AVR, epic-cc PIC18F4550, XC8 if available | arduino/compile-sketches, `pio ci`, make | Every PR |
| Budgets | Flash and RAM per example and tier against `tools/budgets.yaml`; call depth ≤ 3 from `r2s_spin_some` | avr-size, `-fstack-usage`, `-fcallgraph-info`, epic-cc size map and call graph | Every PR |
| Hardware in the loop | Real Uno and PIC18F4550 boards | Self-hosted runner, manual trigger | Phase 3 onwards |

**Key end-to-end scenarios** (in `tests/e2e/`, each a launch_testing test parameterized by MCU backend):

1. Handshake completes; `ros2 topic list` shows the announced topics within 3 s.
2. MCU publishes Float32 at 50 Hz for 10 s; the host receives at least 99% with correct values.
3. Host publishes Twist; the MCU echoes it on a test topic; values match after the narrow round trip.
4. Launch remapping and namespace apply to the MCU's topics.
5. A type hash mismatch rejects only that topic, with an error naming the type.
6. Unplug (close the pty) and replug: CONNECTED again within 3 s without restarting the bridge.
7. MCU reset: the new session ID triggers a re-handshake; publishers are kept when the table is unchanged.
8. Time sync: with a simulated MCU clock drifting 100 ppm, stamps stay within 1 ms of host time after the initial burst.
9. Log messages at each level appear on `/rosout` with the right severity and logger name.
10. Fault injection at 1% byte corruption: no crash, the counters match the injected faults, and delivery recovers on the next frame.

## CI on GitHub Actions

| Workflow | Trigger | Jobs |
| --- | --- | --- |
| `ci.yml` | Push and PR | `lint` (clang-format, clang-tidy, cppcheck, ruff, mypy, ament_lint, vectors_check, actionlint); `core` (gcc, clang, -m32; unit, vectors, sanitizers, coverage); `fuzz-smoke`; `gen` (Python 3.10 to 3.13); `bridge` (colcon build and test in `ros:jazzy` and the newer distros); `e2e-posix` (matrix: distro × `rmw_fastrtps_cpp`, `rmw_cyclonedds_cpp`, `rmw_zenoh_cpp`, with the Zenoh router started first); `e2e-avr`; `build-matrix`; `budgets` (posts a size table as a PR comment and fails on budget breach) |
| `nightly.yml` | Schedule | Long fuzzing, long fault injection, big-endian qemu, extended hypothesis runs, Rolling distro |
| `release.yml` | Tag | Changelog (git-cliff), Python package to PyPI, Arduino library zip, PlatformIO package, epic-hal module bundle |

Rules:

- Every job runs in a pinned container image or pinned action versions. Toolchain versions (avr-gcc, arduino-cli core, epic-cc release, ROS images) are pinned in one file, `ci/versions.env`.
- The ROS jobs use the official `ros:<distro>` images or `ros-tooling/setup-ros` plus `ros-tooling/action-ros-ci`.
- epic-cc is installed from its GitHub release bundle. XC8 builds reuse epic-hal's CI toolchain image if it carries XC8 (research ticket R9). Otherwise XC8 is a manual check.
- Required checks on `main`: everything in `ci.yml` except jobs whose prerequisites haven't landed yet; each such job is marked `continue-on-error` with a link to its ticket until it lands.

## Research tickets

Each one produces a short write-up in `docs/research/` and, where relevant, a proposal to change this document. Until it is done, tickets use the default.

| ID | Question | Deliverable | Default meanwhile |
| --- | --- | --- | --- |
| R1 | rosserial archaeology: features, RAM per pub/sub, time sync, reconnection, failure modes | Feature catalog with source links; updates to the parity table | Parity table in Part I |
| R2 | Prior art: existing rosserial-like projects for ROS 2 | Annotated list and a go/no-go recommendation | Proceed |
| R3 | Can `rosidl_parser` and the RIHS01 algorithm be used without a ROS install (pip), or must they be vendored | Working proof inside `gen/` with a hash test | Vendor the needed modules with attribution |
| R4 | Generic service and client APIs in Jazzy and later distros | API inventory with versions | Introspection-based typed services |
| R5 | simavr: UART-to-pty bridging for ATmega328P, tick source, run in CI | `tests/harness/simavr/` running one echo firmware in CI | — (blocks the e2e-avr job only) |
| R6 | PIC end-to-end: epic-cc's simulator UART support, or epic-hal's host simulation, as the PIC backend for `tests/e2e/` | Recommendation plus a working echo test | Build-only CI for PIC |
| R7 | CRC choice: Hamming distance of CRC-16/CCITT-FALSE and alternatives at our frame sizes (Koopman tables), cost on AVR and PIC18 | Table and recommendation | CRC-16/CCITT-FALSE, bitwise |
| R8 | Serial I/O in the bridge: `transport_drivers` vs asio vs termios; low-latency control on Linux and FTDI | Recommendation with a latency measurement | termios behind an interface |
| R9 | XC8 in CI: license terms and reuse of epic-hal's toolchain image | Working job or a written reason why not | Manual XC8 checks |
| R10 | Arduino `HardwareSerial` behavior: RX buffer size per core, overflow, how to enlarge it via build flags | Notes and port recommendations | Drain into `r2s_rx_feed` in every `r2s_port_poll()` |

## Epics and seed tickets

Ticket IDs are for cross-reference; the ticket agent may renumber. "Deps" lists hard dependencies. Every ticket follows the template and the definition of done below.

### E0: repository bootstrap

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| B1 | Repository skeleton per the layout, LICENSE, README stub, AGENTS.md summarizing Part II | | Layout exists; AGENTS.md links this document |
| B2 | CI skeleton: `ci.yml` with lint jobs and empty test jobs, pinned `ci/versions.env` | B1 | Green run on `main` |
| B3 | Conventional commits check and git-cliff config | B1 | PR title check enforced in CI |
| B4 | Vector schema and `tools/vectors_check.py` | B1 | Invalid vector files fail CI |

### E1: specification and vectors

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| S1 | `spec/protocol.md` v0.1: frame format, CRC, COBS, varint, encoding rules | B1 | Matches Part I and II; reviewed by the maintainer |
| S2 | System channels and state machines in the spec; confirm hash truncation and name hash | S1 | Byte tables for every opcode |
| S3 | Vectors: CRC, COBS, varint | B4, S1 | Includes the published check values |
| S4 | Vectors: Float32, Twist (full and narrow), frames, handshake transcript | S1, G4 | Generated by the reference codec, reviewed |
| S5 | P1 spec: parameters (ID 3) and the reliable channel (ID 6) | S2 | Byte tables and state machines |

### E2: MCU core

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| C1 | Core CMake project, Unity, vector runner, coverage and sanitizer builds | B2 | `ctest` runs in CI with gcc and clang |
| C2 | CRC-16 bitwise | C1, S3 | All CRC vectors pass |
| C3 | COBS encode (streaming) and in-place decode | C1, S3 | All COBS vectors pass; fuzz target added |
| C4 | Varint encode and decode | C1, S3 | Vectors pass; overflow handled |
| C5 | Framer: byte feed, resync on 0x00, CRC check, limits, counters | C2, C3 | Garbage and truncated input tests; fuzz target |
| C6 | Frame transmit: serialize into TX buffer, CRC on the fly, COBS, `r2s_port_write` | C2, C3 | Frame vectors pass |
| C7 | Optional RX ring (`R2S_WITH_RX_RING`) | C1 | Single-producer, single-consumer tests including wraparound |
| C8 | MCU session: ANNOUNCING, CONNECTED, HELLO, TOPIC, TOPIC_STATUS, READY, heartbeat, link timeout | C5, C6, S2 | Handshake transcript vectors pass |
| C9 | Handwritten Float32 and Twist codecs, plus a hand-written topic table, for the vertical slice | C6, S4 | Message vectors pass |
| C10 | Posix port: pty, tick from `clock_gettime`, seed from `/dev/urandom`, a test firmware with configurable topics | C8, C9 | Runs as a process the e2e tests can launch |
| C11 | Fuzz targets and seed corpora wired into `fuzz-smoke` and nightly | C5, C8 | Jobs green |

### E3: generator

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| G1 | Package skeleton, CLI, `version`, packaging for pip and ament_python | B2 | `pip install` and `colcon build` both work |
| G2 | Config schema and `check` with golden error messages | G1 | Each schema rule has a failing-config test |
| G3 | Message loading from search paths and the bundled snapshot; bounds resolution | G1 | Header, Imu, JointState resolve with bounds; unbounded without bounds is an error |
| G4 | Python reference codec, including the narrow profile | G3, S1 | Round-trips; matches S3 vectors |
| G5 | Type hashes (RIHS01) | G3, R3 | Equal to installed type support for all bundled types |
| G6 | C emission: types | G3 | Golden files; compiles with `-std=c99 -pedantic -Werror` |
| G7 | C emission: codecs | G6, G4 | Byte-identical output to C9 for Float32 and Twist; property-based round trip against G4 |
| G8 | C emission: topic table, publish functions, dispatch, limits | G7, C8 | Replaces C9's handwritten table in the posix test firmware |
| G9 | `vectors` subcommand | G4 | Used to produce S4 |
| G10 | Layouts: cmake and arduino | G8 | Example builds in the build matrix |

### E4: bridge

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| H1 | Package skeleton, component, parameters, CI in ROS containers | B2 | `colcon test` green |
| H2 | Serial transport interface; termios implementation; fake transport for tests | H1, R8 | Unit tests with the fake transport |
| H3 | Link the core's framing and CRC; frame vectors pass in C++ | H1, C5, C6 | Vector tests in gtest |
| H4 | Host session state machine and handshake | H2, H3, S2 | Handshake transcript vectors; timeout and retry tests |
| H5 | Type support loading, hash check, TOPIC_STATUS | H4 | Mismatch test rejects one topic only |
| H6 | Packed ⇄ CDR converter with cached plans | H1, G4 | Byte-identical to rclcpp serialization for every vector |
| H7 | Generic publishers and subscriptions, remapping, QoS depth, keep-latest and rate limiting | H5, H6 | Unit tests; e2e scenario 4 |
| H8 | Diagnostics | H4 | Values checked in a unit test |
| H9 | Launch file and example parameter file | H7 | Used by the e2e tests |

### E5: vertical slice end to end (Phase 1 gate)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| V1 | `tests/e2e/` framework, posix backend, scenarios 1 to 4 | C10, H9 | Green in `e2e-posix` for all RMWs |
| V2 | Fault-injecting pty proxy and scenario 10 | V1 | Green |
| V3 | simavr harness | R5 | Echo firmware test green in CI |
| V4 | Uno firmware for the slice; e2e-avr runs scenarios 1 to 4 | V1, V3, G10 | Green |
| V5 | Arduino example sketch and a manual real-Uno checklist | V4 | Maintainer runs it on hardware |

### E6: P0 completion and hardening (Phase 2)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| P1 | Logging end to end | V1 | Scenario 9 |
| P2 | Time: MCU side (PONG, CORRECTION, `r2s_now`) | C8 | Unit tests with wraparound |
| P3 | Time: host estimator and scheduling | H4 | Simulated drift tests; scenario 8 |
| P4 | Reconnect and reset handling | V1 | Scenarios 6 and 7 |
| P5 | Range, Imu, JointState with bounds; error reports | G8 | Vectors and e2e |
| P6 | Budgets: `tools/size_report.py`, `budgets.yaml`, PR comment | V4 | Job fails on a deliberate breach in a test PR |
| P7 | Call-depth check for AVR | V4 | Fails on a deliberate deep call in a test |
| P8 | Big-endian qemu job | C1 | Nightly green |
| P9 | Framing and CRC benchmark (AVR, PIC18) | V4, R7 | Write-up in `docs/benchmarks/` |
| P10 | Encoding benchmark (packed vs CDR vs MAVLink) | G7 | Write-up; decision on narrow per field |
| P11 | Protocol spec v1.0 freeze | P1–P5 | Tagged; vectors frozen |

### E7: PIC, portability and P1 (Phase 3)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| X1 | epic-hal port (PIC18F4550), epic-cc build in CI | V1 | Builds; budgets recorded |
| X2 | PIC end-to-end backend | R6, X1 | Scenarios 1 to 4 on the PIC backend |
| X3 | Compact handshake (MCU, generator node map, bridge loader) | P11 | Scenarios 1 to 4 in compact mode |
| X4 | Tier S on PIC16F193X | X1, X3 | Fits tier S budgets |
| X5 | XC8 build | R9, X1 | Build job or documented manual check |
| X6 | STM32 or RP2040 port | V1 | Builds; e2e on hardware manually |
| X7 | Reliable channel | S5 | Unit and fault-injection tests |
| X8 | Parameters | X7 | `ros2 param set` reaches the MCU in e2e |
| X9 | Services | X7, R4 | Server and client e2e |
| X10 | C++ wrapper (strict EC++), avr-g++ and epic-cc C++ builds | G8 | Wrapper example builds on both; static construction order test |
| X11 | PlatformIO and epic-hal layouts and module entry | G10, X1 | Install-and-build tests in CI |
| X12 | rosidl plugin wrapper | G8 | Generation from `colcon build` in a test package |
| X13 | Cross-compiler demo: same sketch on Uno and PIC18F4550 | X1, X10 | Demo recording |

### E8: release (Phase 4)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| L1 | Release workflow for every artifact | X11 | Dry-run release green |
| L2 | bloom release of the ROS packages | L1 | Packages on the buildfarm |
| L3 | Docs site and "port your rosserial sketch" guide | X10 | Published |
| L4 | Community announcement | L2, L3 | Posted on ROS Discourse |

## Ticket template and definition of done

```markdown
## Context
Why this ticket exists; link to the section of proposal-v2.md.

## Tests first
The tests or vectors to add before the implementation, and what each one proves.

## Acceptance criteria
- [ ] ...

## Out of scope
...

## Dependencies
Tickets that must land first.
```

A ticket is done when:

- the tests were written first and the PR shows them failing before the change;
- all CI jobs that apply are green;
- coverage, budgets and call depth are not regressed;
- `spec/protocol.md` and the vectors are updated if anything on the wire changed;
- user-facing behavior is documented in `docs/` or in the public header comments;
- open questions found along the way are filed as new tickets, not left in code comments.
