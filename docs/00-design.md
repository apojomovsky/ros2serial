# ros2serial design

Oct 20, 2026 · @Alexis Pojomovsky · from the kickoff proposal v3, which superseded v2 (Oct 3, 2026); both are in the git history

> **How to use this document.** Part I is the design: what we build and why. Part II is the implementation specification: repository layout, exact protocol details, APIs, test strategy, CI, research tickets and the seed backlog. An agent creating tickets should start from Part II, section "Epics and seed tickets", and follow the links back into Part I for rationale. Where something is still unknown, Part II names a research ticket and a default to use until it is answered, so no ticket is blocked on a decision. Everything is built test-first, and every test runs on GitHub Actions.

# Part I: Design

## What changed since v2

| Topic | v2 | v3 |
| --- | --- | --- |
| Project ownership | Open (license, home) | Personal project: MIT everywhere, personal repository, community organization only if it grows into one |
| Framing | COBS | Reverse COBS (rCOBS): the MCU streams frames to the UART with no TX buffer |
| CRC | CRC-16/CCITT-FALSE, appended little-endian (breaks the zero-residue check) | CRC-16/MCRF4XX: same polynomial, reflected, so a little-endian append leaves a zero residue |
| Codecs | Generated code per type, nested types flattened | One table-driven codec interpreter plus generated field descriptors in flash |
| `float64` on 32-bit-`double` toolchains | Per-topic opt-in narrow profile, binary64 conversion code otherwise | Narrow is forced by the build; no binary64 conversion code exists on those targets |
| Call depth | "≤ 3, checked in CI" | No fixed number. No recursion and bounded nesting everywhere; RAM stack bytes budgeted on AVR; epic-cc checks the PIC hardware stack itself |
| Code zones | One set of restrictions for everything | Two zones: a portable C99 core (PIC-safe rules) and a comfortable layer (C++ wrapper, runtime registration) that only gcc-class and epic-cc targets compile |
| C++ wrapper | Strict Embedded C++ (no templates, no namespaces), Phase 3 | The epic-cc C++ subset (classes, templates, namespaces; no exceptions, RTTI, heap, virtuals), Phase 1 |
| Declaring topics | `r2s.yaml` plus generator only | Three ways: `r2s.yaml` (generator), an X-macro list in the sketch (no Python), runtime rosserial-style registration (gcc targets) |
| Arduino UX | Generated library copied into the sketch | Library Manager install with bundled common message types; topics declared in the sketch |
| rosserial porting | "Port your rosserial sketch" guide in Phase 3 | rosserial-compatible `ros::NodeHandle` API in Phase 2, with the guide |
| Strings | "In flash" (not true on AVR without `PROGMEM`) | `R2S_FLASH` access macros; flash-string and RAM-string entry points kept separate (AVR and epic-cc both need this) |
| Receive path | Port pushes bytes with `r2s_rx_feed` | Core pulls bytes with `r2s_port_read` inside `spin_some`; dispatch never happens inside the port |
| Transmit path | `r2s_port_write` "may block", yet `R2S_ERR_TX_BUSY` exists | `r2s_port_tx_free()`: a publish that does not fit returns `R2S_ERR_TX_BUSY` before writing anything |
| Session ID | Random from `r2s_port_seed()` (no entropy on a Uno) | Boot ID from the port (EEPROM boot counter on AVR and PIC); unsolicited HELLO means reset |
| Time correction | `drift_ppb`, overflows 32-bit math | `drift_ppm` applied per elapsed millisecond, all 32-bit, with a bounded anchor window |
| Bound-violation report | Names the topic only | Names the topic and the field |
| Tier S reference | PIC16F877A | PIC16F1938 class (enhanced mid-range, linear addressing); 877A becomes a Float32-only stretch goal |
| PIC toolchain | XC8 or a second compiler, Phase 3 | epic-cc first, XC8 optional; all PIC work in the last phase |
| PIC in the plan | Phase 3, before release | Phase 5, after the 1.0 release; PIC-enabling rules apply from day one |
| Distros | Jazzy "and newer" | Jazzy and Lyrical required, Rolling nightly; the generator targets a distro explicitly |
| Uno specifics | Not covered | DTR auto-reset on port open, bootloader delay, ceramic resonator drift, baud error at 16 MHz |
| Phase 0 | Research only | Research plus a throwaway hardware spike to measure real RAM and flash before tickets are written against the budgets |
| Engineering practices | A section of this document | `AGENTS.md` (the rules) and ADR-001 (where they come from), checked against epic-cc's actual tooling |
| Versions | Per component | One version for the repository (bloom releases all packages of a repository with one version) |

## Vision and non-goals

The goal is to offer in ROS 2 what rosserial offered in ROS 1: publishing and subscribing to topics from a small microcontroller, down to 8-bit AVR and PIC, in a few steps and without having to learn DDS or micro-ROS.

The intended user experience is a hobbyist's: install a library, declare a few topics, publish, call spin. The intended engineering standard is a professional's: someone shipping a product on an 8-bit or small 32-bit MCU should be able to choose this library and defend that choice in a design review.

**Goals**

- A user experience equivalent to rosserial, with naming adapted to ROS 2 conventions, and a rosserial-compatible C++ API so existing sketches port in minutes.
- MCU core in C99, with no malloc, no external dependencies and no hardware access, portable across architectures (AVR, PIC, ARM Cortex-M, RISC-V), endianness and compilers (avr-gcc, epic-cc, arm-none-eabi-gcc, XC8 and other C99 compilers).
- Small-part constraint: the core must eventually fit PIC16-class parts (PIC16F1938 class: 1 KB RAM, linear addressing) as well as Uno/Mega-class boards (2 KB tight bound on the Uno). PIC work comes last, but the constraint shapes the core now.
- On the host side, a regular ROS 2 node (Jazzy onwards) that works with any RMW (Fast DDS, Cyclone, rmw_zenoh).
- Transparency: the user writes or reuses `.msg` files and the MCU code is generated, or uses the bundled common types directly.
- A written, versioned protocol specification with test vectors, so third parties can implement it.

**Non-goals**

- Interoperability with MAVLink or other ecosystems.
- Competing with micro-ROS on 32-bit MCUs with an RTOS.
- Actions, full QoS, and taking part in DDS discovery from the MCU.
- Transports other than serial (UART, USB CDC) in the first version.
- ROS 1 or ROS 2 distros older than Jazzy.
- Windows hosts in v1 (Linux first, macOS best effort).

## Hardware tiers

Tiers are defined by resources, not ISA bitness: a 32-bit core is not automatically big.

| Tier | Examples | RAM | Features |
| --- | --- | --- | --- |
| S | PIC16F1938 class (PIC14E, Phase 5); PIC16F877A as a Float32-only stretch goal | 368 B to 1 KB | Publish and subscribe, compact handshake, optional log |
| M | Arduino Uno (ATmega328P) + Mega (ATmega2560), the validation platforms; CH32V00x (Phase 3); PIC18F2455/4550 (Phase 5) | 2 KB on Uno and PIC18F4550, 8 KB on Mega | Everything in P0 and P1 |
| L | STM32 (one small + one mid, Phase 3), RP2040, ESP32 (build-only via the Arduino layout) | 32 KB or more | Everything; full-width `float64` by default |

Features are selected at compile time (`R2S_WITH_TIME`, `R2S_WITH_LOG`, `R2S_WITH_PARAMS`, `R2S_WITH_RELIABLE`, `R2S_WITH_RUNTIME_TOPICS`), so a tier S build carries only publish and subscribe.

Proposed targets, to adjust with the Phase 0 spike and Phase 1 measurements:

| Tier | Core with 1 publisher + 1 subscriber (Float32, Twist) |
| --- | --- |
| S | < 200 B RAM including buffers, < 4K words flash |
| M | < 400 B RAM including buffers, < 4 KB flash (P0 features), measured as the delta over an empty sketch that uses the same serial driver. The K1 spike measured +137 B and +2.6 KB on a Uno build, against +706 B and +9.0 KB for the smallest rosserial configuration (R1) |

## Scope: parity with rosserial

| Feature | Priority | Notes |
| --- | --- | --- |
| Publish and subscribe | P0 | Float32 and Twist first, then Range, Imu and JointState |
| Handshake and topic announcement | P0 | Full mode; compact mode for tier S (Phase 5) |
| Sync loss detection, reconnection, MCU reset detection | P0 | rCOBS resync, heartbeat, boot ID |
| Logging to the host (`/rosout`) | P0 | Non-variadic, per-type API; flash strings |
| Time synchronization (`now()` on the MCU) | P0 | Host-side estimation; see Time |
| C++ API, static topics | P0 | Topics from the X-macro list or `r2s.yaml`; see User API |
| C++ API, rosserial-compatible runtime registration | P0 (Phase 2) | gcc-class targets; see User API |
| Parameters | P1 | Declared by the MCU, exposed as ROS 2 parameters of the bridge node |
| Services (server and client) | P1 | Need the reliable channel |
| TF and diagnostics | P2 | Probably better handled on the host |
| Actions | Out of scope | |

The list is still based on prior knowledge of rosserial; the Phase 0 archaeology validates it against the source code.

## Architecture

```
 build time                                        runtime
 ----------                                        -------
 .msg files ──► r2s-gen (rosidl_parser) ──►  MCU firmware                          host
 r2s.yaml or               │                 ┌──────────────────────┐   serial   ┌─────────────────────┐
 X-macro list              ▼                 │ user code            │  (rCOBS    │ r2s bridge (rclcpp) │
                 C99 headers and sources:    │  └ C++ layer (opt.)  │ ◄─frames─► │  ├ session          │
                 types, field descriptors,   │  └ topic table,      │            │  ├ packed ⇄ CDR      │
                 topic table, typed publish  │     descriptors      │            │  │  (introspection)  │
                 functions, dispatch;        │  └ r2s core (C99)    │            │  └ generic pub/sub  │──► any RMW
                 bundled Arduino library     │     codec interpreter│            └─────────────────────┘
                                             │  └ port glue         │
                                             └──────────────────────┘
```

The host has the message packages installed, as for any ROS 2 node. The MCU only carries the descriptors for the types and topics it declares.

## Two zones: portable core and comfortable layer

The PIC16 constraints are real but should only cost something where PIC actually compiles. The code is split into two zones, and CI enforces the boundary.

| | Portable core | Comfortable layer |
| --- | --- | --- |
| What | Framing, CRC, session, time, log, codec interpreter, C API, static topic table | C++ API, rosserial-compatible runtime registration |
| Language | C99 | The epic-cc C++ subset (see User API) |
| Rules | No function pointers, no recursion, no malloc, no VLAs, no 64-bit C types, no float arithmetic, bounded loops and nesting, buffers that fit one RAM bank on PIC | Templates, namespaces, classes; stored callbacks allowed on gcc-class targets only; no exceptions, RTTI, heap, multiple inheritance or virtuals |
| Compiled by | Every target | AVR, 32-bit gcc targets; static C++ path also on epic-cc |

Why these core rules: on small 8-bit parts an indirect call is lowered to a compare-and-call chain (epic-cc does this on every PIC core, about 10 words per call site per candidate), the hardware call stack is a few levels deep, recursion is a compile error, and floating point is a large software library. None of the rules costs anything measurable on AVR or 32-bit parts.

PIC-enabling rules that apply from day one, even though PIC work comes last:

- no function pointers in the core;
- static topic dispatch through a generated `switch`;
- flash-string and RAM-string entry points kept separate;
- no float arithmetic in the core (floats are copied as bytes);
- every buffer small enough to fit one bank where the target banks RAM.

## Declaring topics

There are three ways to declare topics. All three produce the same wire behavior and run the same conformance and end-to-end tests.

**1. `r2s.yaml` and the generator** (custom messages, bare metal, CMake, PlatformIO, PIC):

```yaml
# r2s.yaml
node: base_controller
distro: jazzy
handshake: full            # or: compact (tier S)
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
    qos: keep_latest
bounds:
  default_string_bound: 16   # optional; without it, unbounded fields are a generator error
  std_msgs/msg/Header.frame_id: 24
  sensor_msgs/msg/JointState.name: [4, 12]   # up to 4 names, 12 chars each
  sensor_msgs/msg/JointState.position: 4
```

**2. An X-macro list in the sketch** (Arduino, no Python involved). The Arduino library ships descriptors for a bundle of common types (`std_msgs`, `geometry_msgs`, `sensor_msgs`) with documented default bounds:

```c
#define R2S_NODE_NAME "base_controller"
#define R2S_TOPICS(X) \
  X(PUB, battery, std_msgs, Float32) \
  X(SUB, cmd_vel, geometry_msgs, Twist)
#include <ros2serial.h>
```

The header expands the list into the topic table, the typed publish functions and the dispatch `switch` inside the sketch's translation unit. The library's own sources (core and bundled descriptors) compile separately and never need the list; the linker drops unused descriptors. Changing the bound of a bundled type means switching to the generator.

**3. Runtime registration, rosserial style** (gcc-class targets only, see User API). Topics are registered while `setup()` runs and live in a RAM table. Registration closes at the first spin; a later registration is an error, never a silent re-handshake.

Whatever the source, the core reads topics through two accessors (`r2s_topic_count()`, `r2s_topic_get()`), backed either by a constant table in flash (ways 1 and 2) or by the RAM table (way 3), selected at link time. No function pointers are involved in the core.

From the topic list the static paths emit:

- message structs and their field descriptors, for the types used only;
- the topic table (IDs, names, type names, hashes, flags) as constant data in flash;
- one typed publish function per published topic, for example `r2s_publish_battery(const std_msgs__msg__Float32 *)`;
- receive dispatch as a `switch (topic_id)` that calls a user-implemented handler for each subscription, for example `r2s_on_cmd_vel(const geometry_msgs__msg__Twist *)`;
- a shared receive buffer (a union of the subscribed types) and frame sizes computed from the largest message in each direction.

Topic names still live in the firmware, in flash, and the launch file can still remap them (see User API).

## Wire encoding: packed, converted on the host

**What the choice is.** The wire encoding is the byte layout of a message on the serial line. DDS uses CDR. CDR aligns every field to its own size, inserting padding bytes, and prepends a 4-byte header. A packed layout writes fields one after the other with no padding. With CDR the bridge could forward bytes unchanged. With packed, the bridge converts.

**Decision: packed little-endian, converted to CDR by the bridge.**

- Lighter on the wire and in RAM: no padding, no encapsulation header.
- More portable: the codec writes each field byte by byte in a fixed order, so it works the same on little- and big-endian MCUs, with or without alignment restrictions, and never `memcpy`s a whole struct.
- The conversion costs almost nothing on the host. The bridge walks the message with `rosidl_typesupport_introspection`, fills a message in memory and serializes it with `rmw_serialize` before publishing through a generic publisher. The bridge needs introspection anyway to handle types it was not compiled against.

The CDR comparison stays in the encoding benchmark (Phase 2) as a check on this decision, not as the deciding input.

**Encoding rules (draft for the spec)**

| `.msg` type | Wire |
| --- | --- |
| `bool`, `byte`, `char`, `int8`, `uint8` | 1 byte |
| `int16` to `int64`, `uint16` to `uint64` | Fixed width, little-endian |
| `float32` | IEEE 754 binary32, little-endian |
| `float64` | IEEE 754 binary64; binary32 when the topic is narrow |
| `string`, `string<=N` | Varint length + bytes, no terminator |
| `T[N]` (fixed array) | N elements, no prefix |
| `T[]`, `T[<=N]` | Varint length + elements |
| Nested message | Fields inline, in declaration order |

`wstring` and `wchar` have no encoding: the generator rejects a message that uses them, naming the field.

Lengths are varints (LEB128): 1 byte for values under 128, which covers almost all bounded fields. As a result **the layout does not depend on the bounds**. The host never needs to know the bound chosen for a field. It only needs to know whether the topic is narrow.

**`float64` and narrow topics.** On a toolchain whose `double` is 32 bits (avr-gcc by default, epic-cc, XC8 in common configurations), sending binary64 carries no extra precision, so every topic is narrow on such a build: all `float64` fields travel as binary32 and the bridge widens them. The build detects this from `<float.h>` (`DBL_MANT_DIG == FLT_MANT_DIG`) and forces the narrow flag in the topic table; no binary64 conversion code exists on these targets. On toolchains with a 64-bit `double` (tier L), the wire carries binary64 by default and a topic can opt into narrow (`narrow_float64: true`) to save bandwidth.

**Floats are bytes in the core.** The core and the codec copy float bytes and never do float arithmetic, so no software float library is pulled in. The only exception is the narrowing cast on tier L opt-in topics, where the hardware or library cost is irrelevant. The build asserts at compile time that `float` is IEEE binary32 (4 bytes, 24-bit mantissa).

**No 64-bit integers in the core.** The core and the codec never use 64-bit C types, because 8-bit toolchains handle them badly (slow, large, or incompletely supported). On toolchains without a usable 64-bit integer, `int64` and `uint64` fields are generated as an 8-byte little-endian array with helper functions.

**Bound violations** are never silently truncated. When publishing, the serializer returns an error. On reception, the decoder drops the frame and reports the topic and the field to the host, which logs a warning naming both. The host has no bounds, so only the MCU checks them; both sides drop a payload that is truncated or has bytes left over.

**Strings carry no `0x00`.** The MCU holds strings NUL-terminated, so a string containing a `0x00` byte would be cut short without notice. Neither side sends one, and both drop a payload that has one; the bridge logs the topic and field instead of publishing a shortened string.

## Protocol draft (v1)

```
frame = rCOBS( topic_id[1] | seq[1] | payload[N] | crc16[2] ) + 0x00
```

| Field | Size | Purpose |
| --- | --- | --- |
| topic_id | 1 byte | 0 to 15 reserved for the system; 16 to 255 for user topics |
| seq | 1 byte | Per-direction, per-topic counter to detect lost frames |
| payload | N bytes | Packed message |
| crc16 | 2 bytes, little-endian | CRC-16/MCRF4XX (polynomial 0x1021 reflected, init 0xFFFF, no final XOR) over topic_id, seq and payload, computed bitwise (no 512-byte table). Appended little-endian, the CRC over the whole frame is 0, so the receiver checks one value |
| rCOBS and delimiter | 2 bytes up to 253 bytes of frame | One code byte (plus one per extra 254 bytes) and the trailing 0x00 |

**Why reverse COBS.** COBS replaces every 0x00 so that 0x00 can mark the end of a frame, but each code byte comes *before* its block, so an encoder must look up to 254 bytes ahead: in practice, a full TX buffer. rCOBS puts each code byte *after* its block. The encoder needs only a counter, so the MCU serializes, computes the CRC and encodes in one pass, straight to the UART. The receiver buffers the frame anyway and decodes it backwards, in place. Overhead and resync behavior are the same as COBS. The exact encoding is in Part II.

**Reserved IDs (draft):** 0 handshake and control, 1 time, 2 log, 3 parameters, 4 heartbeat, 5 error report, 6 reliable-channel ack, 7 to 15 reserved.

**Handshake.** Either side can start it. The MCU sends `HELLO` on boot, and the host sends `HELLO_REQ` when it connects or detects a problem. The MCU's reply contains a header and one entry per topic.

- Header: protocol version, handshake mode, maximum frame size, RX capacity, tick rate, feature bits, node name, and a boot ID that changes on every boot.
- Topic entry, full mode: ID, direction (pub or sub), type name (for example `geometry_msgs/msg/Twist`), the first 8 bytes of the REP-2011 RIHS01 type hash, profile flags (narrow), topic name, and a QoS hint (best effort or reliable, keep-latest).
- Topic entry, compact mode (tier S): ID, direction, type hash, a hash of the topic name, and flags. The generator writes a matching YAML next to the firmware, and the bridge loads it (`node_map:=...`) to recover names and types. The names still come from the firmware's config; they just do not occupy its flash.

The bridge loads the type support for each type name and compares the hash with the one from the installed package. If they differ, it refuses that topic with a clear error, without dropping the session.

**Session, reset and reconnection.** Heartbeats run in both directions. The host treats any of these as an MCU reset and redoes the handshake and the time sync: a different boot ID, an unsolicited `HELLO` while connected, or a `HELLO` sent in reply to a heartbeat. An MCU that is not connected answers any non-handshake host frame with a rate-limited `HELLO`, so a reset is detected within one heartbeat period, without entropy. If the MCU stops seeing host traffic, it stops publishing and returns to announcing.

**Losses and errors.** User topics are best effort. A gap in `seq` increments a counter that the bridge publishes as diagnostics. Parameters, services and the handshake use a small reliable channel (stop-and-wait with ack on ID 6, one outstanding message per direction).

**Flow control.** The MCU announces its RX capacity (the size of its frame buffer). The bridge rate-limits host-to-MCU traffic per topic, caps its total host-to-MCU bandwidth to a fraction of the link, and, for keep-latest topics such as `cmd_vel`, keeps only the most recent message instead of queueing. Topics are serviced round-robin, so one busy topic cannot starve the others.

## Receiving and sending bytes

The core never registers an interrupt or touches a UART. It pulls received bytes from the port inside `r2s_spin_some()` and pushes encoded bytes to the port while publishing:

```c
void r2s_spin_some(void);   /* pull bytes, decode, dispatch, heartbeats, time */
void r2s_rx_push(uint8_t byte);   /* ISR-safe; only with R2S_WITH_RX_RING */
```

**Receive.** `spin_some` reads bytes through `r2s_port_read()` (non-blocking) one at a time until a frame is complete or no data is left, then decodes and dispatches it, up to `R2S_SPIN_MAX_FRAMES` frames per call. Because the core decides how much to read, a complete frame is never overwritten by the next one: unread bytes simply stay in the port's buffer. Handlers run only from `spin_some`, never from inside a publish or an interrupt. The framer records the tick at which each frame's delimiter arrived; the time sync uses it.

Many HALs already keep their own interrupt-driven ring (Arduino's `HardwareSerial`, epic-hal's `lib/serial`). On those, `r2s_port_read` reads from it and the core keeps no second ring, which would waste RAM on a small part.

For ports without a ring, `R2S_WITH_RX_RING` enables a single-producer, single-consumer ring fed by `r2s_rx_push` from the UART interrupt; the core then provides `r2s_port_read` itself. It is safe on any target because:

- The ring is at most 256 bytes, so head and tail are `uint8_t`. A one-byte store is atomic on every MCU we target, so no locks or disabling interrupts are needed.
- Only the ISR writes `head` and only `spin_some` writes `tail`. Both are `volatile`.
- An optional `R2S_MEMORY_BARRIER()` hook (empty by default) covers cores that reorder memory accesses.

rCOBS decoding runs backwards in place in the frame buffer once the delimiter arrives (the decoded frame ends up right-aligned in the buffer), followed by one CRC pass. Neither step needs a second copy.

**Transmit.** A publish first computes the frame's encoded size bound and asks the port for free space with `r2s_port_tx_free()`. If the frame does not fit, the publish returns `R2S_ERR_TX_BUSY` and writes nothing, so the user's loop never blocks on a slow link and a frame is never half-sent. Otherwise the codec serializes field by field into the CRC and the rCOBS encoder, which hands small chunks (`R2S_TX_CHUNK`, default 8 bytes) to `r2s_port_write`. A port with a blocking write and no TX buffer reports `0xFFFF` free space and accepts blocking.

| Port | RX path | TX path |
| --- | --- | --- |
| Arduino (any core) | `r2s_port_read` reads `Serial` | `Serial.write`, `availableForWrite()` as free space |
| AVR bare metal | `R2S_WITH_RX_RING`: `ISR(USART_RX_vect) { r2s_rx_push(UDR0); }` | Blocking write |
| PIC with epic-hal (Phase 5) | `epic_serial_read` | `epic_serial_write`, ring free space |
| STM32, RP2040 | DMA in circular mode plus an idle-line interrupt | DMA or ring |

## Time

**What rosserial did.** Right after connecting, and then every few seconds, the MCU asked the host for the current time. When the reply came, the MCU stored an offset from its own `millis()`, correcting for the round trip. From then on `nh.now()` was that offset plus `millis()`, with millisecond resolution. That is how sketches stamped their messages.

**Why it matters for IMUs.** A 150-byte Imu frame takes about 13 ms to send at 115200 baud. USB-serial adapters add jitter on top: an FTDI adapter's default latency timer is 16 ms, and the Uno's ATmega16U2 and CH340 clones behave differently again. If the bridge stamped messages on arrival, the stamps would be late and irregular. Stamping at the moment of sampling needs `now()` on the MCU.

**Decision: keep rosserial's API, move the hard math to the host.**

1. The bridge periodically sends a ping on ID 1 and the MCU replies with the tick at which the ping's delimiter arrived. Replying is the MCU's only job here.
2. The bridge estimates offset and drift, keeping the samples with the smallest round trip (they have the least delay).
3. The bridge sends the MCU a correction: "tick T0 corresponds to ROS time S0, and the MCU clock is off by D ppm".
4. On the MCU, `r2s_now()` adds the elapsed ticks to S0 and applies `D` once per elapsed millisecond (one millisecond times one ppm is exactly one nanosecond). All arithmetic is 32-bit. The host sends a new correction at least every 5 s and the MCU reports time as unsynced if none arrives within 10 s, which bounds every intermediate value and handles tick wraparound.

The drift term matters: the Uno's ATmega328P runs from a ceramic resonator that can be off by thousands of ppm, which is several milliseconds per second.

The tick comes from the port: `micros()` on Arduino (1 MHz nominal, 4 µs resolution), a 1 ms tick from epic-hal on PIC, a hardware timer elsewhere. The tick rate is announced in the handshake. The bridge also sets the serial port to low latency where the OS supports it. Target accuracy: about 1 ms, bounded by the tick resolution. Phase 2 measures the real figure.

## User API

### C (P0)

One node per firmware, so the API needs no node handle. Names follow rosserial where possible and ROS 2 conventions where they differ (`spin_some`, `msg` in type names). Generated message types reuse the rosidl C naming (`geometry_msgs__msg__Twist`), so ROS 2 users recognize them. They are not ABI-compatible with `rosidl_generator_c`, but the two never get linked together.

```c
#include "r2s_generated.h"        /* from r2s-gen, or the X-macro list */

static std_msgs__msg__Float32 battery;

/* Declared by the generated header; called directly by r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    set_motors(msg->linear.x, msg->angular.z);
}

int main(void)
{
    r2s_init();
    for (;;) {
        battery.data = read_battery();
        (void)r2s_publish_battery(&battery);   /* R2S_ERR_TX_BUSY if the link is full */
        r2s_spin_some();
    }
}
```

Logging is non-variadic, one function per value type, and takes flash strings: `R2S_LOG_INFO("battery low")`, `R2S_LOG_INFO_U16("adc=", v)`. The macros wrap the literal so it stays in flash on AVR (`PSTR`) and pass it unchanged elsewhere. Some 8-bit toolchains cannot read varargs, and a format string costs flash on every target. This matches epic-hal's `epic_serial_put_*` API.

### C++ (P0)

**Compatibility bar: the epic-cc C++ subset.** Classes, constructors and destructors (including for globals), templates, namespaces and operator overloading are allowed. Exceptions, RTTI, the heap, multiple inheritance, virtual functions, the standard library and placement `new` are not. C++11 is the language level (Arduino's default is `gnu++11`). This builds with avr-g++, arm-none-eabi-g++, RISC-V g++, the ESP32 toolchain and epic-cc.

**Static topics (every C++ target, including PIC).** The topic list comes from the X-macro or `r2s.yaml`, and the dispatch `switch` is generated, so every handler call is direct:

```cpp
#define R2S_NODE_NAME "base_controller"
#define R2S_TOPICS(X) \
  X(PUB, battery, std_msgs, Float32) \
  X(SUB, cmd_vel, geometry_msgs, Twist)
#include <ros2serial.hpp>

std_msgs::msg::Float32 battery;

void r2s_on_cmd_vel(const geometry_msgs::msg::Twist &msg) { set_motors(msg.linear.x, msg.angular.z); }

void setup() { r2s::init(); }
void loop()  { battery.data = read_battery(); r2s::pub::battery.publish(battery); r2s::spin_some(); }
```

**rosserial-compatible runtime registration (gcc-class targets, Phase 2).** Existing rosserial sketches port by changing the include:

```cpp
#include <ros2serial/ros.h>
#include <ros2serial/std_msgs/Float32.h>
#include <ros2serial/geometry_msgs/Twist.h>

ros::NodeHandle nh;
std_msgs::Float32 battery;
ros::Publisher battery_pub("battery", &battery);

void on_cmd_vel(const geometry_msgs::Twist &msg) { set_motors(msg.linear.x, msg.angular.z); }
ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub("cmd_vel", &on_cmd_vel);

void setup() { nh.initNode(); nh.advertise(battery_pub); nh.subscribe(cmd_vel_sub); }
void loop()  { battery.data = read_battery(); battery_pub.publish(&battery); nh.spinOnce(); }
```

The objects fill the RAM topic table (about 8 to 10 bytes per topic). The core calls one C hook for runtime dispatch, which the C++ layer implements with a stored callback, so the core itself stays free of function pointers. Each subscriber holds its own message storage, as in rosserial. `nh.loginfo()` accepts both RAM strings and Arduino `F()` strings. `R2S_WITH_RUNTIME_TOPICS` is on by default for gcc toolchains and off for epic-cc and XC8; using the runtime API there is a compile error that points to the static form. It can be re-enabled on PIC18 by anyone who accepts the cost.

**Evidence for the split** (probe on Oct 20, 2026, epic-cc master `0a8828e` with a clang 18 front end, compile only, not simulated): a two-subscriber dispatch compiled to the same flash size in plain C and in C++ with template-instantiated direct calls (599 words on PIC18F4550, 594 on PIC16F1938). The same dispatch with stored callbacks cost 72 more words on PIC18F4550 and 159 more on PIC16F1938, growing with every subscriber. Non-virtual C++, including global constructors, already compiled for the PIC16F1938.

The C++ objects are globals, as in rosserial sketches, which relies on static constructors running before `setup()`. A conformance test checks construction order across source files on every supported compiler.

ROS 2-style aliases (`std_msgs::msg::Float32`) are always provided; rosserial-style aliases (`std_msgs::Float32`) come with the compatibility headers.

### Remapping from a launch file

The bridge creates its publishers and subscriptions under the names the MCU announces. ROS 2 applies remapping and namespace rules when a node creates a topic, so the standard launch syntax works with no extra code:

```python
Node(
    package="r2s_bridge",
    executable="bridge",
    namespace="robot1",
    parameters=[{"port": "/dev/ttyACM0", "baud": 115200}],
    remappings=[("battery", "power/battery")],
)
```

## Code generator and packaging

`r2s-gen` is a standalone Python tool (pip-installable, with no ROS installation required) that reuses `rosidl_parser`. Its inputs are `.msg` files and `r2s.yaml`. Its outputs are C99 headers and sources, the optional C++ headers, and the compact-mode node map. It also builds the bundled Arduino library (common types with default bounds) that the X-macro path uses. Outputs are laid out to be usable as:

- an Arduino library (Library Manager), the default path;
- a PlatformIO library;
- a CMake or Makefile include for bare-metal and vendor toolchains;
- an epic-hal module (serial, tick and EEPROM dependency) for PIC (Phase 5).

The generator targets a ROS distro explicitly (`distro:` in the config, default `jazzy`), because type hashes can differ between distros. It bundles a `common_interfaces` snapshot per supported distro. A thin rosidl plugin wraps it later for users who want generation inside `colcon build`. The bridge, the generator and a bundle of the standard messages are released as ROS packages through the buildfarm.

## Host bridge

- C++ (rclcpp), packaged as a component so several bridges (one per serial port) can share a process.
- Generic publishers and subscriptions, plus introspection-based conversion from packed to CDR.
- Serial I/O behind a small interface; termios first (research ticket R8).
- Board quirks handled explicitly: opening a Uno's port toggles DTR and resets the board, and the bootloader runs for up to about 2 s. The bridge waits for the MCU's unsolicited `HELLO` (up to `boot_wait_ms`) before sending `HELLO_REQ`, so it does not talk to the bootloader. `hupcl:=false` avoids the reset where the adapter allows it.
- MCU log frames are forwarded to the bridge's logger under a child name (`<node>.mcu`), so they appear in `/rosout`.
- Compact mode: loads the node map generated with the firmware.
- P1 parameters: the MCU declares name, type and default in the handshake. The bridge declares them as ROS parameters on its own node, so `ros2 param set` works, and pushes changes to the MCU over the reliable channel.
- P1 services: generic client and service APIs differ by distro (research ticket R4). If they are missing on a supported distro, fall back to introspection-based typed services there.

## Portability notes (AVR now, PIC last)

Validation is Arduino Uno + Mega (AVR) with open-source tools for the first phases. PIC work comes in the last phase, built with epic-cc on epic-hal, but the PIC16 constraints shape the portable core now (see "Two zones").

Constraints that shape the core. Most of them also make it leaner on AVR.

| Constraint | Consequence for ros2serial |
| --- | --- |
| Small hardware call stack on PIC (8 levels classic, 16 on PIC14E; an ISR uses some) | No recursion, bounded nesting, iterative codec; epic-cc rejects programs that overflow the stack |
| Indirect calls lowered to compare-and-call chains on small 8-bit compilers | No function pointers in the core; static dispatch through a generated `switch` |
| No recursion on small-part compilers | Codec interpreter is iterative with an explicit nesting stack of generated depth |
| `double` is 32 bits on default AVR, epic-cc and common PIC toolchains | Narrow forced by the build |
| 64-bit C types slow or incomplete on 8-bit targets | No 64-bit C types in the core or the codec |
| Float is a large software library on 8-bit targets | No float arithmetic in the core |
| No varargs / no stdio on small-part toolchains | Per-type logging API |
| Constant data lives in flash, and on AVR `const` alone does not keep it there | `R2S_FLASH` and `R2S_FLASH_READ_BYTE()` port macros; separate flash-string entry points |
| Banked RAM: on classic PIC14 every object must fit one bank (about 80 bytes) | Tier S reference is PIC14E (linear addressing); 877A limited to small frames |

**Testing.** The conformance vectors run on host builds and on simavr for AVR (328P/2560). PIC end-to-end tests run under MPLAB SIM through epic-hal's mdb gate in Phase 5; until then, an advisory CI job compiles the core with epic-cc so PIC-hostile code is caught early.

## Quality bar

What "professional" means in concrete terms:

- **Specification:** a protocol document versioned independently of the code, with byte-level test vectors for every frame type.
- **Conformance suite:** the same vectors run against the C core (host build, simavr for 328P/2560), the Python reference and the bridge; PIC joins in Phase 5.
- **Fuzzing:** the rCOBS decoder, the CRC, the framer, the session and the codec interpreter (with every descriptor) are fuzzed with libFuzzer on a host build. No input may crash, hang or write out of bounds.
- **C hygiene:** `-std=c99 -pedantic -Wall -Wextra -Werror`, cppcheck and clang-tidy in CI, no undefined behavior (UBSan on host builds). The code follows a MISRA-inspired subset; certification is not a goal.
- **Determinism:** no malloc, no recursion, no function pointers in the core, bounded loops and nesting. Flash, RAM and worst-case stack usage per configuration and per toolchain are reported in CI and tracked over time.
- **C++:** both registration paths compiled in CI with avr-g++ under the subset rules; the static path also with epic-cc from Phase 2 (advisory).
- **Versioning:** semver for the libraries and a protocol version checked in the handshake.
- **Hardware in the loop:** Uno + Mega, starting with the Uno.

## 8-bit gotchas

The tier M budget is 2 KB of RAM (ATmega328P; Mega gives headroom at 8 KB).

- **`double` is 32 bits** on default avr-gcc and small-part compilers. Handled by forced narrow topics.
- **`const` is not flash on AVR.** Constant data and string literals are copied to RAM unless placed with `PROGMEM`. Handled by the `R2S_FLASH` macros and flash-string logging.
- **64-bit integers** are slow on 8-bit cores and incomplete on some toolchains. Kept out of the core; byte-array fallback for message fields.
- **Imu is the stress case:** about 150 bytes narrow and about 300 at full width. Use it to size the defaults and document the trade-off.
- **Bandwidth:** 115200 baud carries about 11.5 KB/s. A narrow Imu at 50 Hz uses about 65% of it, and 100 Hz does not fit. At 16 MHz, 115200 baud has a 2.1% rate error, while 250000, 500000 and 1000000 are exact. 115200 stays the default for compatibility; the faster rates are tested and documented.
- **Arduino's stock RX buffer is 64 bytes**, which fills in about 5.5 ms at 115200. Call spin often, keep host-to-MCU topics keep-latest and rate-limited, or use the bare-metal ISR port.
- **The Uno resets when the port opens** (DTR), then runs its bootloader. The bridge waits for the MCU's `HELLO`.
- **The Uno's 328P clock is a ceramic resonator**, off by up to thousands of ppm. Time sync corrects drift.
- **USB-serial latency:** the FTDI latency timer, the 16U2's USB CDC firmware and CH340 clones affect time sync, not only throughput.
- **No entropy source on small parts.** The boot ID comes from an EEPROM boot counter (one write per boot, far below the endurance limit).

## Plan

Each phase ends in a decision gate and a publishable artifact.

**Phase 0: research and spike (in parallel)**
- *rosserial archaeology:* feature catalog, RAM cost per publisher and subscriber, how time and reconnection really worked, known failure modes. Validates the parity table.
- *Prior art sweep:* ROS Discourse, GitHub, ROBOTIS ros2arduino, micro-ROS on small parts, zenoh-pico over serial, community rosserial-like ports, any ROS 2 host for the original rosserial protocol. Explicit conclusion on whether to proceed.
- *Hardware spike (throwaway, in `scratch/`):* handwritten Float32 and Twist over rCOBS on a real Uno, with a minimal host script. Measures flash, RAM and link behavior against the tier M targets before tickets depend on them.
- *Gate:* go or no-go, tier M targets confirmed or adjusted. *Artifact:* a "state of serial MCUs in ROS 2" post.

**Phase 1: vertical slice (AVR)**
- C core: rCOBS, CRC, minimal session, codec interpreter with handwritten descriptors for Float32 and Twist.
- `r2s-gen` producing those same descriptors plus the topic table and dispatch, compared byte for byte against the handwritten ones; the X-macro header for the bundled types.
- Static C++ API on top of the X-macro list.
- Bridge: handshake (full mode), generic pub/sub, introspection conversion, Uno boot handling.
- CI: host build plus simavr running the conformance vectors.
- *Demo:* teleop `cmd_vel` to a real Uno/Mega and a Float32 back, on Fast DDS and on rmw_zenoh, from a sketch built in the Arduino IDE.
- *Gate:* end-to-end works. *Artifact:* demo video and repository made public.

**Phase 2: measure, harden, rosserial compatibility**
- Framing and CRC benchmark on AVR (Uno/Mega): rCOBS vs COBS vs SLIP vs rosserial style; CRC variants. Flash, RAM, cycles per byte, recovery time.
- Encoding benchmark: packed vs CDR vs MAVLink as a baseline, for Float32, Twist, Range, Imu, JointState.
- P0 complete: logging, time sync with measured accuracy, reconnection and reset detection, Range, Imu, JointState with bounds.
- rosserial-compatible runtime registration and a draft "port your rosserial sketch" guide.
- Fuzzing, the protocol spec v1.0, flash, RAM and stack tracking, the baud-rate matrix, the advisory epic-cc compile job.
- *Gate:* P0 complete and tier M budgets met. *Artifact:* benchmark write-up, the spec, and a rosserial migration post.

**Phase 3: 32-bit ports and P1**
- 32-bit ports: CH32V00x (tier M proof on a modern toolchain) + one small STM32 for tier M/L; RP2040 for tier L; ESP32 build-only via the Arduino layout (no WiFi transport in v1).
- Parameters, services, the reliable channel, PlatformIO packaging, rosidl plugin wrapper.
- *Demo:* the same sketch on Uno, Mega and a 32-bit board, all publishing into the same ROS 2 graph.
- *Gate:* release candidate.

**Phase 4: release and community**
- Buildfarm release of the bridge and generator, Arduino Library Manager entry, docs site, the final "port your rosserial sketch" guide, Discourse announcement.
- Possible move into a community organization (ros-drivers or similar) if the project grows into one.
- *Artifact:* 1.0 release and announcement.

**Phase 5: PIC**
- PIC port on epic-hal, built with epic-cc: tier S on PIC16F1938 class (compact handshake), tier M on PIC18F2455/4550 (C API and static C++).
- PIC end-to-end tests under MPLAB SIM; XC8 builds optional.
- PIC16F877A as a Float32-only stretch goal.
- *Demo:* the same static-API sketch on a Uno and a PIC18, then a PIC16, publishing into the same ROS 2 graph.
- *Artifact:* a cross-board post (ros2serial on epic-hal).

**Role of the agents.** Each spike or workstream gets its own agent. A shared harness (simavr for 328P/2560, host builds, conformance vectors, flash, RAM and stack reports) makes the agents produce comparable tables. A person reads those tables and makes the design decisions.

**Other proposed targets (to adjust with Phase 1 data)**

| Metric | Target |
| --- | --- |
| Framing + CRC on AVR | < 1 KB flash, < 50 B RAM (excluding the RX frame buffer; no TX buffer) |
| Recovery after corruption | Next frame after the next 0x00 |
| Time sync accuracy | about 1 ms at 115200 baud, bounded by the tick resolution, with a 5000 ppm clock error |
| Reset detection | Within one heartbeat period (500 ms by default) |

## Open decisions and risks

Each open decision has a default. Tickets use the default until the decision is made; changing a default later is its own ticket.

| Decision | Default until decided | Decided by |
| --- | --- | --- |
| Project name and C prefix | Repository `ros2serial`, C prefix `r2s_`, ROS packages `r2s_*`. The name was floated in 2018 for an unreleased rosserial rewrite (R2) | Maintainer, before Phase 4 |
| Message type names in C headers (`__` is reserved in C++) | `std_msgs__msg__Float32` in C; C++ code uses namespace aliases | Maintainer, before the C++ API ticket |
| CRC variant | CRC-16/MCRF4XX | Research ticket R7 |
| Type hash truncation on the wire | 8 bytes | Confirmed by spec ticket S2 |
| Narrow per field or per topic (64-bit-`double` targets only) | Per topic | Maintainer, after the Phase 2 encoding benchmark |
| Serial library for the bridge | Plain POSIX `termios` behind a small interface | Research ticket R8 |
| Generic service support per distro | Use generic APIs where present, introspection-based typed services elsewhere | Research ticket R4 |
| Bounds of bundled Arduino types | Strings 16, `frame_id` 16, JointState up to 6 joints | Maintainer, after R1 and the Phase 2 benchmarks |
| Default baud rate | 115200 | Maintainer, after the Phase 2 baud matrix |

Decided since v2: MIT for every component, personal repository, runtime registration on gcc targets only, PIC in the last phase with epic-cc as its primary toolchain.

| Risk | Mitigation |
| --- | --- |
| Scope grows from trying to be universal | Tiers with explicit feature sets; AVR first, PIC last, PIC-enabling rules kept cheap |
| Packed layout and bridge conversion drift apart | Single written spec; the conformance vectors run on all sides in CI |
| The two registration paths drift apart | Every end-to-end scenario runs on both paths |
| rCOBS is less familiar than COBS | Exact spec, vectors, Python reference; the Phase 2 benchmark compares it with COBS |
| Introspection conversion is slow for large messages | Measure in Phase 2; cache per-type conversion plans |
| Config feels like friction compared with rosserial | X-macro path with bundled types, and the rosserial-compatible API |
| 8-bit compiler gaps block the PIC port | epic-cc is maintained by the same author; gaps become epic-cc tickets with minimal repros |
| Prior art exists that was not found | Phase 0 sweep before investing in code |
| Low demand as 32-bit MCUs get cheaper | Validate early; the value is the experience and robustness, not only size, and the core also runs on 32-bit MCUs |

---

# Part II: Implementation specification

## Repository layout

One repository, one colcon workspace root. Every directory has its own tests next to its code.

```
ros2serial/
├── AGENTS.md                 conventions for agents and contributors (summarizes this Part II)
├── Makefile                  only entry point: bootstrap, image, shell, test, pre-pr-check, help
├── Dockerfile                base/dev/ci(=dev)/release stages; versions pinned via ci/versions.env
├── .githooks/                pre-commit, commit-msg, pre-push (installed by make setup-hooks)
├── scripts/                  ci-test.sh, pre-pr-check.sh, prose-diff.sh, lint-python.sh, helpers + test_*.py
├── LICENSE                   MIT
├── spec/
│   ├── protocol.md           the normative protocol specification (grows from Part I + this Part II)
│   └── vectors/              conformance vectors (YAML), the single source of truth for every codec test
│       ├── crc/  rcobs/  varint/  frames/  messages/  session/  time/
├── core/                     C99 MCU core (no ROS, no hardware)
│   ├── include/r2s/          public headers: r2s.h, r2s_config.h, r2s_port.h, r2s_arch.h, r2s_codec.h; r2s_crc.h, r2s_rcobs.h, r2s_varint.h and r2s_framer.h, shared with the bridge
│   ├── src/                  crc.c, rcobs.c, varint.c, framer.c, session.c, time.c, log.c, ring.c, codec.c, topics.c
│   ├── tests/                Unity unit tests, vector runner, CMakeLists.txt
│   └── fuzz/                 libFuzzer targets and seed corpora
├── cpp/                      C++ layer: static API (ros2serial.hpp) and rosserial-compatible API (ros.h)
├── gen/                      r2s-gen: Python package (pip) and ament_python package (ROS)
│   ├── r2s_gen/              cli.py, config.py, msgs.py, hashing.py, descriptors.py, reference/ (Python reference codec), snapshots/, templates/
│   └── tests/                pytest: unit, golden-file, property-based
├── bridge/
│   ├── r2s_bridge/           C++ rclcpp component + executable, launch/, config/
│   └── r2s_bridge_tests/     launch_testing integration tests (ROS package, test-only)
├── ports/
│   ├── posix/                host "virtual MCU" over a pty: used by integration tests and fuzzing
│   ├── arduino/              Arduino library glue (Serial, micros, EEPROM boot ID)
│   ├── avr/                  bare-metal AVR glue (USART ISR, timer tick, EEPROM boot ID)
│   ├── ch32v/                Phase 3
│   ├── stm32/                Phase 3: one small STM32 + RP2040; ESP32 build-only via the Arduino layout
│   └── epic-hal/             Phase 5: epic-hal serial, tick and EEPROM glue for PIC
├── examples/                 one directory per example, each built in CI
├── tests/
│   ├── e2e/                  shared end-to-end scenarios, run against posix and simavr (MPLAB SIM in Phase 5)
│   └── harness/              simavr UART-to-pty harness, fault-injecting pty proxy
├── tools/                    size_report.py, stack_report.py, budgets.yaml, vectors_check.py, zone_check.py
├── scratch/                  gitignored per-ticket experiments; nothing ships
├── docs/                     user documentation (later the docs site)
└── .github/workflows/
```

**Code reuse between MCU and host.** The bridge links `core/src/crc.c`, `rcobs.c`, `varint.c` and `framer.c` compiled for the host. Framing, CRC and lengths therefore have a single implementation. The host session logic is separate C++.

**The rCOBS encoder is a stream.** `r2s_rcobs_encode_byte()` takes one byte and writes the bytes it produces, at most two, so the MCU serializes, computes the CRC and encodes in one pass with no TX buffer. `r2s_rcobs_encode_end()` writes the last code byte; the delimiter is the framer's. The decoder takes the encoded frame in a buffer and writes the decoded frame over it, right aligned, so it needs no second copy.

## Engineering conventions

| Area | Convention |
| --- | --- |
| C (core) | C99, `-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow`, `clang-format` (config in repo), `stdint.h` types only, no `int`/`long` in interfaces, no 64-bit types, no function pointers, no recursion, no malloc, no VLAs, no float arithmetic, no `printf` |
| C++ (MCU layer) | C++11, the epic-cc subset (see Part I), `-fno-exceptions -fno-rtti`, no virtuals, stored callbacks only under `R2S_WITH_RUNTIME_TOPICS` |
| Zone boundary | `tools/zone_check.py` fails CI if anything under `core/` includes from `cpp/`, uses a function pointer type, or calls a float operation (checked on the object files) |
| C++ (bridge) | C++17 (Jazzy's baseline), `ament_lint_auto` (cpplint, uncrustify or clang-format, cppcheck), ROS 2 style |
| Python | 3.10 or newer, `ruff` (lint + format), `mypy --strict` on `r2s_gen`, `pytest`, `hypothesis` |
| Commits | Conventional Commits (`feat(core): ...`, `fix(bridge): ...`, `test(gen): ...`); changelog with git-cliff; details in `AGENTS.md` |
| Branches and PRs | `master` is the default branch; one ticket per PR in a `.worktrees/` worktree, never on `master`; branches named `<type>/<issue>-<slug>`; a draft PR is the claim; each PR is squashed into one commit on `master`, and the PR title is that commit's subject; no PR is stacked on another (`docs/adr/ADR-003-squash-merges.md`); CI must be green; the PR body carries `Closes #N` and a `## Review` section (see `docs/01-testing.md`) |
| Versions | One semver for the whole repository (`VERSION`, bumped only by `cut-release.yml`) plus the protocol version (`R2S_PROTOCOL_VERSION`, starts at 1) |
| Licenses | MIT for everything |
| Docs | Every public function has a doc comment; every PR that changes behavior updates `spec/protocol.md` or `docs/` in the same PR |

## Engineering practices

The workflow, tooling and writing rules (Makefile, Docker image, hooks, worktrees, review gate, takeoff ritual, commit hygiene, CI workflows, releases, comment and docs rules) are in `AGENTS.md`, the normative rulebook. Most of them come from epic-cc; `docs/adr/ADR-001-engineering-practices.md` says what was adopted, adapted or left out, and why.

## Protocol details

`spec/protocol.md` is the normative form of this section, with an ID on every rule; where the two differ, the spec wins and this section is fixed. All multi-byte integers are little-endian. "string" means varint length followed by bytes, without a terminator. Sizes are in bytes. Varints are at most 3 bytes and 65535, in their shortest form only.

### Frame and limits

- `frame = rCOBS(topic_id u8 | seq u8 | payload | crc16 u16) 0x00`, as in Part I.
- `crc16` is CRC-16/MCRF4XX (width 16, polynomial 0x1021, reflected input and output, init 0xFFFF, no final XOR; check value of `"123456789"` is `0x6F91`) over `topic_id`, `seq` and `payload`, stored little-endian. The receiver computes the same CRC over the whole decoded frame, CRC included, and accepts it when the result is 0.
- `seq` is one counter per `topic_id` per direction. It starts at 0 after each handshake and wraps at 255. A gap on a user topic counts lost frames but does not drop the frame.
- The maximum unencoded frame (`topic_id` to `crc16`) is set at build time (`R2S_MAX_FRAME`, generated) and announced in `HELLO`. The protocol allows up to 65277, so the encoded frame and its delimiter fit in 65535 bytes and a 16-bit index. The host never sends a frame larger than the MCU announced; the host's own limit is 65277 and is not announced. A receiver buffers up to the encoded bound of its limit (`n + n / 254 + 1`).
- A frame shorter than 4 bytes after decoding, with a bad CRC, with an invalid rCOBS code, or longer than `R2S_MAX_FRAME`, is dropped and counted. A decoded frame over the limit that still fits the encoded buffer counts in `dropped_frames` alone, because `rx_overflows` counts the buffer bound ([HB-6]). Below a limit of 254 the case cannot arise: the encoding of a longer frame is always over the bound.
- A receiver starts as if it had just seen a `0x00`, and two consecutive `0x00` bytes (an empty frame) are ignored without counting, so a sender may send a `0x00` before a frame to flush a partial one off the line.

### rCOBS

Encoding of a byte string `D` (the delimiter is appended afterwards by the framer):

```
n = 1
for each byte b of D:
    if b == 0:  emit n; n = 1
    else:       emit b; n = n + 1
                if n == 0xFF: emit 0xFF; n = 1
emit n
```

Decoding of an encoded string `E` (without its delimiter) runs from the end. Read the last byte as a code `c` (0, or more than `c - 1` bytes before it, is an error; a last code of 0xFF is an error too); the `c - 1` bytes before it are data; the group before it, if any, is followed by a 0x00 in the output unless its own code is 0xFF. Writing the output from the end of the buffer backwards decodes in place, with the result right-aligned.

The encoding is deterministic: 254 non-zero bytes at the end of a frame produce a trailing empty group (`... FF 01`). Decoders accept exactly this encoding.

### System channels

The first payload byte of IDs 0 and 1 is an opcode.

**ID 0, control**

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | HELLO_REQ | host → MCU | `protocol_version u8` |
| 0x02 | HELLO | MCU → host | `protocol_version u8, handshake_mode u8 (0 full, 1 compact), boot_id u32, max_frame u16, rx_capacity u16, tick_hz u32, features u8, topic_count u8`, then `node_name string` (full) or `node_name_hash u32` (compact) |
| 0x03 | TOPIC_REQ | host → MCU | `index u8` (0 to topic_count − 1) |
| 0x04 | TOPIC | MCU → host | `index u8, topic_id u8, flags u8, type_hash[8]`, then `type_name string, topic_name string` (full) or `topic_name_hash u32` (compact) |
| 0x05 | TOPIC_STATUS | host → MCU | `topic_id u8, status u8 (0 ok, 1 unknown type, 2 hash mismatch, 3 name conflict)` |
| 0x06 | READY | host → MCU | none |

- `features` bits: 0 time, 1 log, 2 params, 3 reliable channel, 4 `double` is 32 bits (informational), 5 runtime topic registration (informational). The other bits are reserved and must be 0.
- `flags` bits: 0 direction (0 = the MCU publishes, 1 = the MCU subscribes), 1 narrow_float64, 2 keep_latest, 3 reliable. The other bits are reserved. The flags are the truth for the wire; bit 1 is always set on a build whose `double` is 32 bits.
- `rx_capacity` is the size of the MCU's RX frame buffer.
- `tick_hz` must be `1000 × k` with `k` a divisor of 10^6 (1 kHz, 10 kHz, 100 kHz and 1 MHz are typical), so the MCU converts ticks to nanoseconds with integer math.
- Name hashes in compact mode are FNV-1a 32-bit over the UTF-8 name. Confirmed by spec ticket S2.
- Each topic is requested and sent one at a time, so a tier S MCU never needs a buffer larger than one TOPIC frame.

**ID 1, time**

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | PING | host → MCU | `ping_id u8` |
| 0x02 | PONG | MCU → host | `ping_id u8, tick u32` (tick at which the PING frame's delimiter arrived) |
| 0x03 | CORRECTION | host → MCU | `tick0 u32, sec i32, nanosec u32, drift_ppm i16` |

MCU time computation, all in 32-bit integers (`k = tick_hz / 1000` ticks per millisecond, `p = 10^6 / k` nanoseconds per tick):

```
e   = tick - tick0                       (unsigned, wraps correctly)
ms  = e / k;  r = e % k
if ms > R2S_TIME_MAX_ANCHOR_MS: time is unsynced
sec = sec0 + ms / 1000
ns  = nanosec0 + (ms % 1000) * 1000000 + r * p     (fits u32, < 2 * 10^9)
normalize ns into [0, 10^9) by carrying into sec
ns  = ns + ms * drift_ppm                          (|ms * drift_ppm| <= 2 * 10^8, fits i32)
normalize again
```

`R2S_TIME_MAX_ANCHOR_MS` defaults to 10000 and `drift_ppm` is limited to ±20000. `drift_ppm` is the number of nanoseconds of ROS time to add per nominal millisecond of MCU time.

**ID 2, log** (MCU → host): `level u8` (ROS severities: 10 debug, 20 info, 30 warn, 40 error, 50 fatal), `text string`, `value_kind u8` (0 none, 1 u16, 2 i16, 3 u32, 4 i32, 5 f32), then the value if present.

**ID 3, parameters**: P1, specified by ticket S5.

**ID 4, heartbeat**: host → MCU has an empty payload. MCU → host carries `rx_frames u16, crc_errors u16, rx_overflows u16, dropped_frames u16`, all wrapping counters. `rx_frames` counts the frames the MCU accepted, `crc_errors` the ones it dropped for a bad CRC, `rx_overflows` the ones that did not fit its buffer, and `dropped_frames` every frame it dropped, whichever rule dropped it.

**ID 5, error report** (MCU → host): `code u8, topic_id u8, count u16, field u8`. Codes: 1 bound violation on decode, 2 unknown topic ID, 3 RX overflow, 4 frame too large, 5 publish while not connected (reported once per connection), 6 TX busy (reported once per second at most). `count` is the number of times the error occurred since the last error report of that code, and it wraps at 65535; a code that is rate-limited is counted the same way and reported no more often than that. `field` is the index of the offending field in depth-first declaration order over the type's field definitions (array elements are not counted separately), or 0xFF when not applicable. The bridge computes the same order with introspection and logs the field path.

**ID 6, reliable-channel ack**: P1, specified by ticket S5.

### State machines

**MCU**

```
BOOT ──init, send HELLO──► ANNOUNCING ──READY──► CONNECTED
                              ▲   │                  │
                              │   └─ every 1 s with no host traffic: send HELLO
                              └──────── link timeout, or HELLO_REQ received ─┘
```

- ANNOUNCING: replies to HELLO_REQ with HELLO and to TOPIC_REQ with TOPIC; stores TOPIC_STATUS per topic; answers any other host frame (heartbeat, user topic) with HELLO, at most once per 100 ms; ignores user topic frames otherwise; `r2s_publish_*` returns `R2S_ERR_NOT_CONNECTED` and sends nothing.
- CONNECTED: sends a heartbeat every `R2S_HEARTBEAT_MS` (default 500); publishing works for topics with status ok.
- Link timeout: no valid frame from the host for `R2S_LINK_TIMEOUT_MS` (default 2000).
- `boot_id` comes from `r2s_port_boot_id()` once per boot.

**Host**

```
DISCONNECTED ──port open──► BOOT_WAIT ──HELLO or boot_wait_ms──► HANDSHAKING ──all topics resolved, READY sent──► CONNECTED
     ▲                                                               ▲                                               │
     └── port error ─────────────────────────────────────────────────┴──── link timeout or reset detected ────────────┘
```

- DISCONNECTED: tries to open the port every `reconnect_period_ms` (default 1000).
- BOOT_WAIT: after opening the port, waits for an unsolicited HELLO for up to `boot_wait_ms` (default 2500) before sending anything, so a board reset by DTR is not talking to its bootloader.
- HANDSHAKING: sends HELLO_REQ every 500 ms until HELLO arrives. Rejects a protocol version it does not support, and logs an error naming both versions. Then, for each index, sends TOPIC_REQ with a 200 ms timeout and 3 retries. Resolves the type support, checks the hash, sends TOPIC_STATUS, creates or reuses the publisher or subscription, and finally sends READY.
- CONNECTED: heartbeat every 500 ms, time sync (see below), forwarding.
- Reset detected: a HELLO arrives while connected, or a HELLO with a different `boot_id`.
- On re-handshake, publishers and subscriptions are kept if the new topic table is identical to the previous one. Otherwise they are destroyed and recreated.

**Time sync on the host.** Right after READY, 8 PINGs 50 ms apart. Then one PING every `time_sync.period_ms` (1000 for the first 10 s, then the parameter's value, default 5000). The estimator keeps a sliding window of the 16 most recent samples, uses the lowest round trip ones for the offset, and a linear fit for drift. It sends a CORRECTION after the initial burst, whenever the estimate moves by more than 100 µs, and at least every 5 s.

## Codec descriptors

The generator emits, for each message type used, a type record and a list of field entries, as constant data in flash. One interpreter in `core/src/codec.c` encodes and decodes any type from them. It is iterative, keeps an explicit nesting stack of `R2S_MAX_NEST` frames (generated: the deepest nesting among the types used), and reads descriptors through `R2S_FLASH_READ_BYTE()`.

**Type record:** `size u16` (`sizeof` of the C struct, for array strides), `first_field u16`, `field_count u8`.

**Field entry (8 bytes):**

| Field | Meaning |
| --- | --- |
| `kind u8` | BOOL, U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, STR, NEST |
| `mode u8` | 0 scalar, 1 fixed array, 2 bounded array; bit 7 set when the bounded size field is `uint16_t` |
| `offset u16` | `offsetof` the field in its C struct |
| `count u16` | Array length (fixed) or bound (bounded) |
| `aux u16` | String bound for STR, type index for NEST |

The exact byte layout, the order of entries and the validation rules (bool must be 0 or 1, lengths within bounds, strings NUL-terminated on decode) are specified by ticket S6. The descriptors never appear on the wire, so they can change without a protocol version bump.

## C API reference

### Public headers

```c
/* r2s.h */
typedef int8_t r2s_status_t;          /* 0 ok, negative errors */
#define R2S_OK                    0
#define R2S_ERR_NOT_CONNECTED    (-1)
#define R2S_ERR_BOUNDS           (-2)
#define R2S_ERR_TX_BUSY          (-3)   /* not enough TX space; nothing was written */
#define R2S_ERR_TOPIC_REJECTED   (-4)

typedef struct { int32_t sec; uint32_t nanosec; } r2s_time_t;   /* builtin_interfaces/Time */

void r2s_init(void);
void r2s_spin_some(void);
void r2s_rx_push(uint8_t byte);                         /* R2S_WITH_RX_RING only, ISR-safe */
uint8_t r2s_connected(void);
r2s_time_t r2s_now(void);                               /* R2S_WITH_TIME only; {0,0} while unsynced */
uint8_t r2s_time_synced(void);                          /* R2S_WITH_TIME only */

/* R2S_WITH_LOG only; one family per level: debug, info, warn, error, fatal.
   r2s_fstr_t points to flash; use the R2S_LOG_* macros, which wrap literals. */
void r2s_log_info(r2s_fstr_t text);
void r2s_log_info_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_info_i16(r2s_fstr_t text, int16_t v);
void r2s_log_info_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_info_i32(r2s_fstr_t text, int32_t v);
void r2s_log_info_f32(r2s_fstr_t text, float v);
void r2s_log_info_ram(const char *text);                /* text built at runtime, in RAM */
#define R2S_LOG_INFO(lit)          r2s_log_info(R2S_FSTR(lit))
#define R2S_LOG_INFO_U16(lit, v)   r2s_log_info_u16(R2S_FSTR(lit), (v))
```

```c
/* r2s_arch.h: selected by compiler and target, overridable */
#define R2S_FLASH                  /* PROGMEM on AVR, empty elsewhere */
#define R2S_FLASH_READ_BYTE(p)     /* pgm_read_byte(p) on AVR, *(p) elsewhere */
#define R2S_FSTR(lit)              /* PSTR(lit) on AVR, (lit) elsewhere */
typedef const char *r2s_fstr_t;
```

```c
/* r2s_varint.h: lengths as unsigned LEB128 (spec/protocol.md, section 5).
   Not part of the firmware API: the codec uses it, and so does the bridge,
   which reads and writes the lengths of a packed payload. */
#define R2S_VARINT_MAX 0xFFFFu
#define R2S_VARINT_MAX_BYTES 3u

#define R2S_VARINT_OK 0
#define R2S_VARINT_ERR_OVERFLOW (-1)     /* VAR-2 */
#define R2S_VARINT_ERR_TRUNCATED (-2)    /* VAR-3 */
#define R2S_VARINT_ERR_NOT_SHORTEST (-3) /* VAR-4 */

uint8_t r2s_varint_encode(uint16_t value, uint8_t *out);
int8_t r2s_varint_decode(const uint8_t *data, uint16_t len, uint16_t *value, uint8_t *used);
```

```c
/* r2s_rcobs.h: the framing that removes every 0x00 from a frame
   (spec/protocol.md, section 4). Not part of the firmware API: the core's
   framing uses it, and so does the bridge. */
#define R2S_RCOBS_OK 0
#define R2S_RCOBS_ERR_CODE (-1)     /* COB-5 */
#define R2S_RCOBS_ERR_TRAILING (-2) /* COB-6 */

#define R2S_RCOBS_MAX_EMIT 2u

typedef struct { uint8_t run; } r2s_rcobs_encoder_t;

void r2s_rcobs_encoder_init(r2s_rcobs_encoder_t *enc);
uint8_t r2s_rcobs_encode_byte(r2s_rcobs_encoder_t *enc, uint8_t byte, uint8_t *out);
uint8_t r2s_rcobs_encode_end(r2s_rcobs_encoder_t *enc, uint8_t *out);
int8_t r2s_rcobs_decode(uint8_t *buf, uint16_t len, uint8_t **decoded, uint16_t *decoded_len);
```

### Port contract

Each port implements these as ordinary functions, resolved at link time:

```c
/* r2s_port.h */
uint16_t r2s_port_read(uint8_t *buf, uint16_t max);       /* non-blocking; returns bytes read (core provides it with R2S_WITH_RX_RING) */
void     r2s_port_write(const uint8_t *buf, uint16_t len); /* must accept len bytes when tx_free said so */
uint16_t r2s_port_tx_free(void);                          /* bytes writable without blocking; 0xFFFF if blocking is acceptable */
uint32_t r2s_port_tick(void);                             /* monotonic, wraps; rate = R2S_TICK_HZ */
uint32_t r2s_port_boot_id(void);                          /* differs on every boot: EEPROM counter on AVR/PIC, random on tier L */
```

The core calls `r2s_port_read` with `max = 1` while it assembles a frame, so it never consumes bytes beyond the current delimiter.

### Generated API

For each topic in the list (generator or X-macro):

```c
r2s_status_t r2s_publish_<name>(const <Type> *msg);   /* published topics */
void r2s_on_<name>(const <Type> *msg);               /* subscribed topics: prototype generated, user implements */
```

`<name>` is the topic name with `/` replaced by `_`. A topic name that cannot be mapped to a valid C identifier, or two names that map to the same identifier, is a generator error.

### C type mapping

| `.msg` | C |
| --- | --- |
| `bool` | `uint8_t` (0 or 1; any other value on the wire is a decode error) |
| `byte`, `char`, `uint8` | `uint8_t` |
| `int8` to `int32`, `uint16` to `uint32` | `int8_t` to `uint32_t` |
| `int64`, `uint64` | `int64_t`, `uint64_t`; with `R2S_NO_INT64`, `r2s_i64_t`/`r2s_u64_t` (8-byte arrays plus helpers) |
| `float32` | `float` |
| `float64` | `double` (32 bits on default avr-gcc, epic-cc and common PIC toolchains, where the topic is narrow) |
| `string<=N` | `char name[N + 1]`, NUL-terminated |
| `T[N]` | `T name[N]` |
| `T[<=N]` | `struct { uint8_t size; T data[N]; }` (`uint16_t size` if N > 255) |
| Nested message | Nested struct |

## Code generator specification

### Command line

```
r2s-gen generate --config r2s.yaml --out <dir> [--msg-path <dir>]... [--layout cmake|arduino|platformio|epic-hal] [--cpp]
r2s-gen bundle   --distro <name> --out <dir>                    # the Arduino library with bundled types
r2s-gen check    --config r2s.yaml [--msg-path <dir>]...       # validate only, exit code 0 or 1
r2s-gen vectors  --type <pkg/msg/Type> --message <yaml>         # print payload hex from the reference codec
r2s-gen version
```

### Message sources

The generator searches `--msg-path` directories first, then `share/*/msg` under `AMENT_PREFIX_PATH` when ROS is sourced, and finally the bundled snapshot of `common_interfaces` and `rcl_interfaces` for the configured distro (Apache 2.0, with attribution), so it works without ROS. When ROS is sourced and its distro differs from the configured one, the generator warns.

### Config schema

`r2s.yaml` is validated against a JSON Schema shipped in the package (`r2s_gen/schema/r2s.schema.json`). Keys:

| Key | Type | Required | Default |
| --- | --- | --- | --- |
| `node` | string, valid ROS node name | yes | |
| `distro` | `jazzy`, `lyrical`, `rolling` | no | `jazzy` |
| `handshake` | `full` or `compact` | no | `full` |
| `tier` | `S`, `M` or `L` (selects defaults and budget checks) | no | `M` |
| `features` | list of `time`, `log`, `params`, `reliable`, `rx_ring` | no | `[time, log]` |
| `topics[].name` | relative ROS topic name | yes | |
| `topics[].type` | `pkg/msg/Type` | yes | |
| `topics[].direction` | `pub` or `sub` | yes | |
| `topics[].qos` | `best_effort`, `keep_latest`, `reliable` | no | `best_effort` |
| `topics[].narrow_float64` | bool; only meaningful on 64-bit-`double` builds, forced on otherwise | no | `false` |
| `bounds` | map of `pkg/msg/Type.field` to an int, or `[count, string_bound]` | no | |
| `bounds.default_string_bound` | int | no | none (unbounded fields are errors) |
| `limits.max_frame` | int, overrides the computed value | no | computed |

Generator errors are precise and actionable: file, key or field path, what is wrong, and how to fix it. Each error message has a golden test.

### Outputs

- `r2s_generated.h` and `r2s_generated.c`: types, descriptors, topic table, publish functions, dispatch, `R2S_MAX_FRAME`, `R2S_MAX_NEST`, feature macros, the protocol version.
- `r2s_generated.hpp` with `--cpp`.
- `r2s_node_map.yaml` in compact mode.
- A `layout`-specific wrapper: `library.properties` (Arduino), `library.json` (PlatformIO), a module entry (epic-hal, Phase 5), `CMakeLists.txt` (cmake).
- Every output file starts with a header naming the generator version and config hash. Output is deterministic: same inputs, byte-identical files.

### Type hash

The generator computes the REP-2011 RIHS01 hash for each type. Default approach: reuse `rosidl_generator_type_description`'s algorithm (research ticket R3 decides whether it is imported or vendored). A test compares the generator's hash with the one in the installed type support for every bundled type, on every supported distro.

## Bridge specification

### Executable and component

- Package `r2s_bridge`, component `r2s_bridge::Bridge`, executable `bridge`.
- One instance per serial port. `/dev/serial/by-id/...` paths are recommended in the docs.

### Parameters

| Parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `port` | string | `/dev/ttyACM0` | Serial device |
| `baud` | int | 115200 | Baud rate |
| `low_latency` | bool | true | Request low-latency mode on the port when supported |
| `hupcl` | bool | true | Keep the default DTR behavior (resets a Uno on open); false avoids the reset where the adapter allows it |
| `boot_wait_ms` | int | 2500 | How long to wait for an unsolicited HELLO after opening the port |
| `node_map` | string | empty | Path to `r2s_node_map.yaml` (compact mode) |
| `reconnect_period_ms` | int | 1000 | |
| `heartbeat_period_ms` | int | 500 | |
| `link_timeout_ms` | int | 2000 | |
| `time_sync.enabled` | bool | true | |
| `time_sync.period_ms` | int | 5000 | Steady-state period, at most 5000 |
| `rate_limit.default_hz` | double | 0 (none) | Default host-to-MCU rate limit per topic |
| `rate_limit.overrides` | string array | empty | Per-topic limits as `"<topic>=<hz>"`, for example `"cmd_vel=20"` |
| `tx_budget_pct` | int | 50 | Cap on host-to-MCU bandwidth, as a percentage of the link |
| `qos_depth` | int | 10 | Depth for publishers and subscriptions created for the MCU |

### Published by the bridge

- `/diagnostics` (via `diagnostic_updater`): link state, boot ID, protocol version, MCU counters from heartbeats, sequence gaps per topic, time sync offset, drift and round trip.
- The MCU's topics, under the names it announced, after remapping and namespacing.
- MCU log messages through the bridge's logger (`<node_name>.mcu`), so they reach `/rosout`.

### Implementation pointers

- Generic publishers and subscriptions: `rclcpp::Node::create_generic_publisher` and `create_generic_subscription`.
- Type support at runtime: `rclcpp::get_typesupport_library` and `rclcpp::get_typesupport_handle`, loading both `rosidl_typesupport_introspection_cpp` (for the field layout) and `rosidl_typesupport_cpp` (for `rmw_serialize`).
- Installed type hash: the `get_type_hash_func` member of the message type support.
- Packed to CDR: build the message in memory with the introspection `init_function`, fill it field by field, then `rmw_serialize`. CDR to packed for subscriptions: `rmw_deserialize`, then walk the fields. Conversion plans are built once per type and cached. The same walk yields the depth-first field order used by error reports.

## Test strategy

The full strategy is `docs/01-testing.md`, which is normative. In short:

- **Test-first, from the spec down.** Behavior that crosses the wire is fixed by a conformance vector before any code; the C core, the Python reference and the bridge all run the same vectors. Every normative statement in the spec carries an ID, and CI fails when an ID has no test.
- **Designed for testability.** One static state struct per core module with a test-only reset, link-time seams (port functions, user handlers) with a scriptable fake port, time only through the port tick, pure functions for the algorithms, a thin rclcpp shell around plain C++ classes in the bridge, a functional pipeline in the generator.
- **Levels:** unit, component, contract (vectors), integration (bridge plus a posix virtual MCU), system (simavr, then hardware), plus property-based, model-based, differential, fuzz, fault-injection, golden, soak and budget tests where they fit.
- **Extent:** coverage floors per component (95% lines for the core), 100% of spec IDs and error paths, and mutation testing nightly as the real measure of test quality.
- **When:** seconds while editing, under 30 s at commit, under 15 min per PR, hours nightly, plus the hardware checklist at release.
- **Honesty:** flaky tests are bugs, tests are reviewed before the code, golden files and vectors change only on purpose, and the fakes have tests of their own.

Scenario numbers in the tickets below refer to the key end-to-end scenarios in `docs/01-testing.md`, section 10.

## CI on GitHub Actions

| Workflow | Trigger | Jobs |
| --- | --- | --- |
| `ci.yml` | Push and PR | `lint` (clang-format, clang-tidy, cppcheck, ruff, mypy, ament_lint, vectors_check, zone_check, actionlint); `core` (gcc, clang, -m32; unit, vectors, sanitizers, coverage); `fuzz-smoke`; `gen` (Python 3.10 to 3.14); `cpp`; `bridge` (colcon build and test in `ros:jazzy` and `ros:lyrical`); `e2e-posix` (matrix: distro × `rmw_fastrtps_cpp`, `rmw_cyclonedds_cpp`, `rmw_zenoh_cpp`, with the Zenoh router started first); `e2e-avr`; `build-matrix`; `pic-canary` (advisory, from Phase 2); `budgets` (size table as a PR comment, fails on budget breach) |
| `nightly.yml` | Schedule | Long fuzzing, long fault injection, big-endian qemu, extended hypothesis runs, Rolling distro |
| `release.yml` | Tag | Changelog (git-cliff), Python package to PyPI, Arduino library zip, PlatformIO package, epic-hal module bundle (Phase 5) |

Rules:
- Every job runs in a pinned container image or pinned action versions. Toolchain versions (avr-gcc, arduino-cli core, ROS images, epic-cc bundle) are pinned in one file, `ci/versions.env`.
- The ROS jobs use the official `ros:<distro>` images or `ros-tooling/setup-ros` plus `ros-tooling/action-ros-ci`.
- PR comments from fork PRs are posted by a separate `workflow_run` job, since a fork PR's token cannot write comments.
- The self-hosted hardware runner only runs `workflow_dispatch` jobs started by maintainers. It never runs code from pull requests.
- Required checks on `master`: everything in `ci.yml` except advisory jobs and jobs whose prerequisites haven't landed yet; each such job is marked `continue-on-error` with a link to its ticket until it lands.

## Research tickets

Each one produces a short write-up in `docs/research/` and, where relevant, a proposal to change this document. Until it is done, tickets use the default.

| ID | Question | Deliverable | Default meanwhile |
| --- | --- | --- | --- |
| R1 | rosserial archaeology: features, RAM per pub/sub, time sync, reconnection, failure modes, the exact C++ API surface sketches use | Feature catalog with source links; updates to the parity table and the compatibility API | Parity table in Part I |
| R2 | Prior art: rosserial-like projects for ROS 2, micro-ROS on small parts, zenoh-pico over serial, ROS 2 hosts for the original rosserial protocol | Annotated list and a go/no-go recommendation | Proceed |
| R3 | Can `rosidl_parser` and the RIHS01 algorithm be used without a ROS install (pip), or must they be vendored | Working proof inside `gen/` with a hash test | Vendor the needed modules with attribution |
| R4 | Generic service and client APIs per supported distro | API inventory with versions | Generic where present, introspection-based typed services elsewhere |
| R5 | simavr: UART-to-pty bridging for ATmega328P/2560, tick source, EEPROM, stack painting, run in CI | `tests/harness/simavr/` running echo firmware (Uno + Mega) in CI | None needed: blocks only the e2e-avr job |
| R6 | PIC end-to-end under MPLAB SIM through epic-hal's mdb gate: UART input and output, tick, EEPROM (Phase 5) | Working echo test in CI | Build-only PIC canary |
| R7 | CRC choice: Hamming distance of CRC-16/MCRF4XX and alternatives at our frame sizes (Koopman tables), cost on AVR | Table and recommendation | CRC-16/MCRF4XX, bitwise |
| R8 | Serial I/O in the bridge: `transport_drivers` vs asio vs termios; low-latency control on Linux; latency and DTR behavior of the 16U2, CH340 and FTDI adapters | Recommendation with latency measurements | termios behind an interface |
| R9 | XC8 builds for PIC (license terms, toolchain image), as a secondary compiler (Phase 5) | Working job or a written reason why not | epic-cc only |
| R10 | Arduino `HardwareSerial` behavior: RX buffer size per core, overflow, `availableForWrite()`, how to enlarge buffers via build flags | Notes and port recommendations | Read byte by byte in `r2s_port_read` |
| R11 | CH32V00x: which part is the tier M proof (RAM and flash of the V002, V003, V006) and its toolchain | Part choice with datasheet links | CH32V003 class, 2 KB RAM |
| R12 | Static C++ API on epic-cc for PIC14E and PIC18: verify the compile-time results in simulation, global constructors included | Simulated fixtures and an epic-cc ticket for any gap | Advisory compile-only canary |
| R13 | Mutation testing tools for C, C++ and Python (Mull, mutmut, alternatives): accuracy, speed, CI fit | Recommendation with a trial run on the core and the generator | Mull and mutmut |

## Epics and seed tickets

Ticket IDs are for cross-reference; the ticket agent may renumber. "Deps" lists hard dependencies. Every ticket follows the template and the definition of done below.

### E00: Phase 0

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| K0 | R1 and R2 write-ups and the go/no-go note | | Maintainer decision recorded in `docs/research/` |
| K1 | Hardware spike on a real Uno: handwritten Float32 and Twist over rCOBS, minimal host script, flash and RAM measured (lives in `scratch/`, not merged) | | Measurements recorded; tier M targets confirmed or adjusted in this document |

### E10: test infrastructure

Tickets T1 to T8 are specified in `docs/01-testing.md`, section 11, with their dependencies on the epics below. T1 lands before any core ticket after C1, and T8 before any bridge ticket after H1.

### E0: repository bootstrap

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| B1 | Repository skeleton per the layout, LICENSE, README stub, `AGENTS.md` and ADR-001 from the engineering practices draft; this document moves to `docs/00-design.md` | | Layout exists; AGENTS.md links the design document |
| B2 | CI skeleton: `ci.yml` with lint jobs and empty test jobs, `scripts/ci-test.sh` as the single suite script, pinned `ci/versions.env` | B1 | Green run on `master`; `make test` runs the same script as CI |
| B3 | Conventional commits check and git-cliff config | B1 | PR title check enforced in CI |
| B4 | Vector schema and `tools/vectors_check.py` | B1 | Invalid vector files fail CI |
| B5 | Makefile entry points + Docker dev/ci/release images + `make bootstrap`/`doctor` | B1 | Clean clone to `make test` per AGENTS.md; `dev` and `ci` identical |
| B6 | Git hooks + `make pre-pr-check` (takeoff) + `scripts/prose-diff.sh` + `scripts/lint-python.sh` + `scripts/push.sh` | B5 | Ritual fails a deliberately dirty PR in a test run |
| B8 | `scripts/next_ticket.py` (claim rule), `pr-body.yml`, `cut-release.yml` | B2 | Picks only unblocked, unclaimed, non-conflicting tickets in a test fixture; release dry run green |
| B7 | `tools/zone_check.py` | B2 | Fails on a deliberate function pointer and a deliberate float operation in `core/` |

### E1: specification and vectors

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| S1 | `spec/protocol.md` v0.1: frame format, CRC, rCOBS, varint, encoding rules | B1 | Matches Part I and II; reviewed by the maintainer |
| S2 | System channels, time math and state machines in the spec; confirm hash truncation and name hash | S1 | Byte tables for every opcode |
| S3 | Vectors: CRC, rCOBS, varint | B4, S1 | Includes the published check value and the rCOBS edge cases (empty, all zeros, 254 and 255 non-zero bytes) |
| S4 | Vectors: Float32, Twist (full and narrow) by hand first, then frames and a handshake transcript from the reference | S1, G4 | Hand vectors reviewed; reference reproduces them |
| S5 | P1 spec: parameters (ID 3) and the reliable channel (ID 6) | S2 | Byte tables and state machines |
| S6 | Descriptor format spec (type records, field entries, validation rules) | S1 | Reviewed; used by C9 and G7 |
| S7 | Time vectors: wraparound, maximum drift, anchor expiry | S2 | Reference and C agree |

### E2: MCU core

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| C1 | Core CMake project, Unity, vector runner, coverage and sanitizer builds | B2 | `ctest` runs in CI with gcc and clang |
| C2 | CRC-16/MCRF4XX bitwise | C1, S3 | All CRC vectors pass, zero residue tested |
| C3 | rCOBS streaming encode and backward in-place decode | C1, S3 | All rCOBS vectors pass; fuzz target added |
| C4 | Varint encode and decode | C1, S3 | Vectors pass; overflow handled |
| C5 | Framer: pull through `r2s_port_read`, resync on 0x00, CRC check, limits, counters, delimiter tick | C2, C3 | Garbage and truncated input tests; a waiting frame is never overwritten; fuzz target |
| C6 | Frame transmit: `r2s_port_tx_free` check, streaming serialize, CRC and rCOBS, chunked `r2s_port_write` | C2, C3 | Frame vectors pass; TX_BUSY writes nothing |
| C7 | Optional RX ring (`R2S_WITH_RX_RING`) providing `r2s_port_read` | C1 | Single-producer, single-consumer tests including wraparound |
| C8 | MCU session: ANNOUNCING, CONNECTED, HELLO rules, TOPIC, TOPIC_STATUS, READY, heartbeat, link timeout, boot ID | C5, C6, S2 | Handshake transcript vectors pass |
| C9 | Codec interpreter plus handwritten descriptors and topic table for Float32 and Twist | C6, S4, S6 | Message vectors pass; nesting stack bounded |
| C10 | Posix port: pty, tick from `clock_gettime`, boot ID from a file counter, a test firmware with configurable topics | C8, C9 | Runs as a process the e2e tests can launch |
| C11 | Fuzz targets and seed corpora wired into `fuzz-smoke` and nightly | C5, C8, C9 | Jobs green |
| C12 | `r2s_arch.h` flash macros, flash-string logging API and `R2S_LOG_*` macros | C1 | AVR build keeps strings out of RAM (checked with avr-size) |
| C13 | Topic accessors with the flash-table backend | C8 | Unit tests; the runtime backend arrives with P11 |

### E3: generator

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| G1 | Package skeleton, CLI, `version`, packaging for pip and ament_python | B2 | `pip install` and `colcon build` both work |
| G2 | Config schema and `check` with golden error messages | G1 | Each schema rule has a failing-config test |
| G3 | Message loading from search paths and the per-distro snapshots; bounds resolution | G1 | Header, Imu, JointState resolve with bounds; unbounded without bounds is an error |
| G4 | Python reference codec, rCOBS and CRC, including narrow topics | G3, S1 | Round-trips; matches S3 vectors and the hand-made S4 vectors |
| G5 | Type hashes (RIHS01), per distro | G3, R3 | Equal to installed type support for all bundled types |
| G6 | C emission: types | G3 | Golden files; compiles with `-std=c99 -pedantic -Werror` |
| G7 | C emission: descriptors | G6, S6 | Byte-identical to C9's handwritten descriptors for Float32 and Twist; property-based round trip against G4 |
| G8 | C emission: topic table, publish functions, dispatch, limits, name collision checks | G7, C8 | Replaces C9's handwritten table in the posix test firmware |
| G9 | `vectors` subcommand | G4 | Used to produce S4 |
| G10 | Layouts: cmake and arduino | G8 | Example builds in the build matrix |
| G11 | `bundle` subcommand and the X-macro header for the Arduino library | G8, G10 | Sketch with an X-macro list builds in arduino-cli for Uno and Mega |

### E4: bridge

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| H1 | Package skeleton, component, parameters, CI in ROS containers | B2 | `colcon test` green on Jazzy and Lyrical |
| H2 | Serial transport interface; termios implementation with `hupcl` and low latency; fake transport for tests | H1, R8 | Unit tests with the fake transport |
| H3 | Link the core's framing and CRC; frame vectors pass in C++ | H1, C5, C6 | Vector tests in gtest |
| H4 | Host session state machine: boot wait, handshake, reset rules | H2, H3, S2 | Handshake transcript vectors; timeout, retry and reset tests |
| H5 | Type support loading, hash check, TOPIC_STATUS | H4 | Mismatch test rejects one topic only |
| H6 | Packed ⇄ CDR converter with cached plans and depth-first field order | H1, G4 | Byte-identical to rclcpp serialization for every vector |
| H7 | Generic publishers and subscriptions, remapping, QoS depth, keep-latest, rate limiting, bandwidth cap, round-robin | H5, H6 | Unit tests; e2e scenario 4 |
| H8 | Diagnostics | H4 | Values checked in a unit test |
| H9 | Launch file and example parameter file | H7 | Used by the e2e tests |

### E5: vertical slice end to end (Phase 1 gate)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| V1 | `tests/e2e/` framework, posix backend, scenarios 1 to 4 | C10, H9 | Green in `e2e-posix` for all RMWs |
| V2 | Fault-injecting pty proxy and scenario 10 | V1 | Green |
| V3 | simavr harness | R5 | Echo firmware test green in CI |
| V4 | Uno + Mega firmware for the slice; e2e-avr runs scenarios 1 to 4 on both | V1, V3, G10 | Green |
| V5 | Static C++ API (`ros2serial.hpp`) over the X-macro list and the generator | G11, C13 | Uno and Mega sketches build; scenarios 1 to 4 pass with a C++ firmware |
| V6 | Arduino example sketches and a manual real-Uno + real-Mega checklist | V4, V5 | Maintainer runs both on hardware, built from the Arduino IDE |

### E6: P0 completion, hardening and rosserial compatibility (Phase 2)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| P1 | Logging end to end | V1, C12 | Scenario 9 |
| P2 | Time: MCU side (PONG with delimiter tick, CORRECTION, `r2s_now`) | C8, S7 | Unit tests with wraparound, drift and anchor expiry |
| P3 | Time: host estimator and scheduling | H4 | Simulated drift tests; scenario 8 |
| P4 | Reconnect and reset handling | V1 | Scenarios 6 and 7 |
| P5 | Range, Imu, JointState with bounds; error reports with field index | G8, H6 | Vectors; scenario 11 |
| P6 | Budgets: `tools/size_report.py`, `tools/stack_report.py`, `budgets.yaml`, PR comment | V4 | Job fails on a deliberate breach in a test PR |
| P7 | Stack painting in simavr during the e2e scenarios | V4, R5 | Measured worst case reported next to the static figure |
| P8 | Big-endian qemu job | C1 | Nightly green |
| P9 | Framing and CRC benchmark (AVR Uno/Mega): rCOBS, COBS, SLIP, rosserial style | V4, R7 | Write-up in `docs/benchmarks/` |
| P10 | Encoding benchmark (packed vs CDR vs MAVLink) | G7 | Write-up; decision on narrow per field |
| P11 | rosserial-compatible runtime registration: RAM topic table backend, `ros::NodeHandle` and `NodeHandle_<...>` (size parameters accepted and ignored), `Publisher`, `Subscriber<T>` and `Subscriber<T, ObjT>` (member functions), `setSpinTimeout`, `ros::Time` and `ros::Duration` arithmetic, `tf::TransformBroadcaster`, compatibility headers (see R1) | V5, C13 | Every e2e scenario also passes on the runtime path; rosserial example sketches with fixed-size messages build unmodified apart from includes; arrays, strings and `Header` need the one-line edits the migration guide documents; services fail to compile with a clear message until X6 |
| P12 | Link saturation and TX_BUSY | C6, V1 | Scenario 12 |
| P13 | Baud-rate matrix: 115200, 250000, 500000, 1000000 in simavr and on hardware | V4 | Results documented; default confirmed or changed |
| P14 | PIC canary job with epic-cc (advisory) | C9, V5 | Core and static C++ compile for PIC16F1938 and PIC18F4550 |
| P15 | Draft "port your rosserial sketch" guide | P11 | Reviewed by the maintainer |
| P16 | Protocol spec v1.0 freeze | P1 to P5, P12 | Tagged; vectors frozen |

### E7: 32-bit ports and P1 (Phase 3)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| X1 | CH32V00x port (tier M proof) | V1, R11 | Builds; budgets recorded; e2e on hardware manually |
| X2 | One small STM32 port (tier M/L) and RP2040 (tier L) | V1 | Builds; budgets recorded; e2e on hardware manually |
| X3 | ESP32 build-only via the Arduino layout | G10 | In the build matrix |
| X4 | Reliable channel | S5 | Unit and fault-injection tests |
| X5 | Parameters | X4 | `ros2 param set` reaches the MCU in e2e |
| X6 | Services | X4, R4 | Server and client e2e |
| X7 | PlatformIO layout | G10 | Install-and-build tests in CI |
| X8 | rosidl plugin wrapper | G8 | Generation from `colcon build` in a test package |
| X9 | Cross-board demo: same sketch on Uno, Mega and a 32-bit board | X1 or X2, V6 | Demo recording |

### E8: release (Phase 4)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| L1 | Release workflow for every artifact | X7 | Dry-run release green |
| L2 | bloom release of the ROS packages | L1 | Packages on the buildfarm |
| L3 | Arduino Library Manager submission | L1, G11 | Library installable from the IDE |
| L4 | Docs site and the final "port your rosserial sketch" guide | P15 | Published |
| L5 | Community announcement | L2, L3, L4 | Posted on ROS Discourse |

### E9: PIC (Phase 5)

| ID | Ticket | Deps | Acceptance |
| --- | --- | --- | --- |
| Y1 | epic-hal port glue: serial, tick, EEPROM boot ID | P14 | Builds with epic-cc for PIC16F1938 and PIC18F4550 |
| Y2 | PIC end-to-end backend under MPLAB SIM | R6, Y1 | Scenarios 1 to 4 on the PIC backend |
| Y3 | Compact handshake (MCU, generator node map, bridge loader) | P16 | Scenarios 1 to 4 in compact mode |
| Y4 | Tier S budget proof on PIC16F1938 | Y1, Y3 | Fits tier S budgets |
| Y5 | Tier M on PIC18F2455/4550: C API and static C++ | Y1, R12 | Scenarios 1 to 4 |
| Y6 | epic-hal layout and module entry | G10, Y1 | Install-and-build test in CI |
| Y7 | PIC canary becomes a required job | Y1 | Required on `master` |
| Y8 | XC8 build (optional) | R9, Y1 | Build job or documented reason why not |
| Y9 | PIC16F877A stretch: Float32-only within one-bank buffers | Y4 | Builds and passes scenario 2, or a written reason why not |
| Y10 | Cross-board demo: the same static-API sketch on a Uno, a PIC18 and a PIC16 | Y4, Y5 | Demo recording |

## Ticket template and definition of done

```markdown
## Context
Why this ticket exists; link to the section of the design document.

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

- the tests were written first and fail before the change;
- all CI jobs that apply are green;
- coverage, budgets and the zone check are not regressed;
- `spec/protocol.md` and the vectors are updated if anything on the wire changed;
- user-facing behavior is documented in `docs/` or in the public header comments;
- open questions found along the way are filed as new tickets, not left in code comments.
