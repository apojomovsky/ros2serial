# rosserial for ROS 2: kickoff document

Oct 2, 2026 · @Alexis Pojomovsky

## Vision and non-goals

The goal is to offer in ROS 2 what rosserial offered in ROS 1: publishing and subscribing to topics from a small microcontroller, including an 8-bit AVR, in a few steps and without having to learn DDS or micro-ROS.

The problem: micro-ROS is the official replacement, but it requires 32-bit MCUs and a heavy toolchain (an agent on the host, a library compiled per board). The request to port rosserial to ROS 2 was [closed as "not planned"](https://github.com/ros-drivers/rosserial/issues/589), on the grounds that rosserial relied on reusing the ROS 1 serialization as is.

**Goals**

- A user experience equivalent to rosserial: include the message header, declare a publisher or subscriber, call spin.
- MCU core in C99, with no malloc and no external dependencies, portable to AVR and PIC.
- On the host side, a regular ROS 2 node that works with any RMW (Fast DDS by default, Cyclone, rmw\_zenoh).
- Transparency: the user writes `.msg` files and the conversions are generated at build time.

**Non-goals**

- Interoperability with MAVLink or other ecosystems.
- Competing with micro-ROS on 32-bit MCUs with an RTOS.
- Actions, full QoS, and taking part in DDS discovery from the MCU.
- Transports other than serial (UART, USB CDC) in the first version.

## Scope: parity with rosserial

Parity is split into levels, and P0 is the minimum for an end-to-end demo with a real Arduino.

| Feature | Priority | Notes |
| --- | --- | --- |
| Publish and subscribe (fixed-size types) | P0 | Float32 and Twist first, then Range and Imu |
| Handshake and topic negotiation | P0 | Rosserial [negotiates with TopicInfo messages](http://wiki.ros.org/rosserial/Overview/Protocol); here, plus a compiled-table mode |
| Sync loss detection and reconnection | P0 | Resynchronization on a delimiter (COBS) |
| Logging to the host | P0 | Equivalent to rosserial's loginfo and warn |
| Time synchronization | P0 | Simplified NTP-style timestamp exchange |
| Parameters read from the host at startup | P1 | Avoids recompiling to change constants |
| Services (server and client) | P1 | After the framing is stable |
| TF and diagnostics | P2 | Probably better handled on the host |
| Actions | Out of scope |  |

This list comes from prior knowledge of rosserial and from the protocol documentation. Spike 1 validates it against the actual source code before the scope is committed.

## Architecture

The protocol is split into four layers (framing, integrity, serialization and session), and the user only touches the `.msg` file: everything else is generated at build time or reused.

&#91;embedded content: architecture · build time and runtime\]

Top: what happens during `colcon build`; bottom: what runs at runtime. The bridge node is a regular ROS 2 node, so it works with any RMW, and the MCU never speaks DDS.

## Protocol draft (v0)

The proposed frame costs about 6 bytes of overhead against rosserial's 8, and it resynchronizes on the next delimiter instead of relying on the length.

```
frame = COBS( topic_id[1] | seq[1] | payload[N] | crc16[2] ) + 0x00
```

| Field | Size | Purpose |
| --- | --- | --- |
| topic\_id | 1 byte | 0 to 15 reserved for the system (handshake, time, log, parameters); 16 to 255 for user topics |
| seq | 1 byte | Per-direction counter to detect lost frames |
| payload | N bytes | Serialized message; native CDR as the starting point |
| crc16 | 2 bytes | Covers topic\_id, seq and payload; exact variant to be decided in spike 2 |
| COBS and delimiter | \~2 bytes | One encoding byte and the trailing 0x00; the message length comes from the frame itself |

**Handshake, two modes**

1. Runtime negotiation: the host requests the topic list on ID 0 and the MCU replies with the name, type and schema hash of each one, as rosserial does.
2. Compiled table: the generator emits the same ID to name, type and hash map both in the firmware and in a host-side YAML. The MCU only knows IDs, which saves flash and RAM.

In both modes the schema hash is verified on connect, to detect a message whose definition has changed. It is the same idea MAVLink uses with its CRC\_EXTRA and that rosserial solved with the md5sum.

**Losses and errors**

v0 has no retransmission. A gap in `seq` is reported to the host and high-frequency topics tolerate it. Services (P1) will need their own retry mechanism.

| Protocol | Fixed overhead per message | Resynchronization |
| --- | --- | --- |
| rosserial | 8 bytes ([7 of header and 1 of checksum](http://wiki.ros.org/rosserial/Overview/Protocol)) | By length and sum checksum |
| Proposed v0 | \~6 bytes (estimated) | Next 0x00 |
| [MAVLink 2](https://mavlink.io/en/about/overview.html) | \~12 bytes (approximate, includes sysid and compid) | Magic marker plus CRC |

The v0 and MAVLink values are estimates; spike 2 measures them with real frames.

## Reuse decisions

No algorithms are invented and as much ROS 2 infrastructure as possible is reused; the zero-dependency rule applies only to the core that runs on the MCU.

| Layer | Decision | What is reused | Why |
| --- | --- | --- | --- |
| Framing | Existing algorithm, own code or a small library | COBS | Standard and a few lines long; a portable C99 implementation avoids being tied to Arduino or avr-libc |
| Integrity | Existing algorithm, own code | CRC-16 (variant to be decided) | Standard math; validated against known test vectors |
| Serialization | Existing standard, own generator | CDR | Avoids translating on the host when the layout matches |
| Session | Own | Nothing | It is the part specific to the problem |
| Code generator | Reuse | rosidl plugin | Already handles .msg parsing, bounded types and the colcon build integration |
| Host bridge | Reuse | rclcpp or rclpy, rosidl introspection, a standard serial library | Regular ROS 2 code, no memory limits |

**Criteria for adopting a library instead of writing one**

1. Portable: C99 with no platform dependencies, suitable for compilers such as avr-gcc, XC8 and SDCC.
2. Fits the flash and RAM budget measured in spike 2.
3. Permissive license (MIT, BSD or Apache 2.0), compatible with a contribution to the community.
4. Active repository with tests.

COBS or SLIP libraries for Arduino, such as PacketSerial, are candidates to evaluate in spike 2. Their maintenance status and license were not verified.

## 8-bit gotchas

The reference budget is an [ATmega328P with 32 KB of flash and 2 KB of RAM](https://newscrewdriver.com/2020/08/05/notes-on-ros2-and-rosserial/), and almost all technical risks come from that scale.

- **4-byte `double`:** in avr-gcc `double` is a 32-bit float, but many ROS messages use `float64` (for example Imu). We must choose between converting to 8-byte IEEE 754 on the wire or defining a "narrow" profile where the host widens.
- **64-bit integers:** no efficient support on 8 bits; they should be measured and their use limited.
- **Strings and sequences:** bounded types are required in the `.msg` (`string<=32`, `float32[<=8]`) and the generator fails at compile time if the bound is missing.
- **CDR alignment and endianness:** AVR and PIC are little endian and have no alignment restrictions; the padding needs to be checked against rclpy.
- **Compilers:** XC8 and SDCC differ from avr-gcc in the size of `double` and in C99 support; they are tested only once AVR is stable. Their current details were not verified.
- **Buffer budget:** the maximum frame size and the number of topics are fixed at compile time and reported to the host in the handshake.

## Spike plan

Seven research and prototyping spikes, each with a measurable deliverable; the numeric thresholds are proposed targets to adjust with the first data.

| # | Spike | Deliverable | Proposed success criterion |
| --- | --- | --- | --- |
| 1 | Rosserial archaeology | Feature catalog, RAM cost per publisher and subscriber, failure modes | Parity list validated against the source code |
| 2 | Framing and CRC benchmark on AVR | Table of flash, RAM, cycles per byte and recovery time after corruption (COBS vs SLIP vs rosserial style; CRC-8, bitwise CRC-16 and nibble-based CRC-16) | Framing plus CRC in under 1 KB of flash and under 50 bytes of RAM |
| 3 | Encoding benchmark | Bytes on the wire, flash and cycles for Float32, Twist, Imu, Range and JointState with CDR, packed layout and MAVLink as a baseline | CDR vs packed decision backed by data |
| 4 | Host bridge | Node with generic publishers and subscribers that forwards already-serialized bytes | Float32 round trip working with Fast DDS and with rmw\_zenoh |
| 5 | Prior art sweep | Annotated list of existing projects on ROS Discourse and GitHub | Explicit conclusion on whether to proceed |
| 6 | rosidl plugin | C99 header generated in colcon build for Float32 and Twist | Bytes identical to rclpy's in automated tests |
| 7 | Introspection bridge | Publish a message built at runtime from wire bytes | Support a new type without recompiling the bridge |

**Suggested order.** Spikes 1 and 5 are reading and research and run in parallel. Then 6 and 4 together: if the plugin and the bridge work with Float32 and Twist, there is an end-to-end demo.

**Role of the agents.** One agent per spike with a shared harness (CI tests with simavr for AVR, byte-for-byte comparison against rclpy) that produces tables with the same metrics. Design decisions are made by a person reading those tables; real hardware-in-the-loop is added only for the boards one wants to certify.

## Open decisions and risks

The most constraining decision is native CDR versus an own packed layout, and the data to make it comes from spike 3.

- [ ] Native CDR or packed layout (initial inclination: CDR, for the near-transparent bridge)
- [ ] `float64` on 8 bits: convert to 8 bytes on the wire or a "narrow" profile
- [ ] Bridge in C++ (rclcpp) or in Python (rclpy), depending on spike 4
- [ ] Project name, and whether it lives as a personal repository or inside a community organization
- [ ] License (Apache 2.0 is the usual one in the ROS 2 ecosystem)

| Risk | Mitigation |
| --- | --- |
| Scope grows from trying to be universal | Small portable core with AVR as the reference; other MCUs as later ports |
| The rosidl plugin turns out harder than expected | Alternative: a standalone generator script invoked from CMake |
| Prior art exists that was not found | Spike 5 before investing in code |
| Low demand as 32-bit MCUs get cheaper | Validate with the community early; the core value is the user experience, not just size |

## Milestones and public visibility

The project moves in four phases, and each one ends in a decision gate and in something that can be published to build presence in the community.

&#91;embedded content: milestones · 4 phases, 3 gates\]

Dates are set after spike 1, once the real effort is known. The publishable content is a proposal: what matters is that each phase leaves an artifact that is useful on its own, even if the project does not continue. The benchmarks from phases 1 and 2 serve the community regardless of how the rest turns out.

