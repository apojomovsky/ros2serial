# Encoding benchmark: packed vs CDR vs MAVLink

P10 benchmark for the Phase 2 encoding decision. The shipped packed
encoding and the wire stay as they are; this write-up records the numbers
and recommends keeping the per-topic narrow default. The `needs-decision`
label stays on #144 for the maintainer's call.

Scope: the nine bundled types, on representative payloads taken from the
spec vectors (wide profile plus its narrow pair where narrowing changes
anything). Byte counts are deterministic. Timing comes from simavr cycles,
never wall clock.

Method:

- Packed sizes come from `benchmark/encoding/sizes.py`, an independent
  model of the wire rules in `spec/protocol.md`, pinned to the reviewed
  vector payloads by `tools/test_encoding_benchmark.py`.
- CDR sizes come from the ROS container's own `rmw_serialize` (Fast DDS,
  Jazzy), probed per payload by `benchmark/encoding/cdr_probe.py`. An
  analytic model cross-checks every figure and the test fails on any
  mismatch. One rule the probe taught the model: member alignment counts
  from the post-header start, and sequence content aligns to its element
  size even when the sequence is empty.
- MAVLink-style sizes reorder each message's fields largest first, size
  strings and sequences at the bundle's documented bounds, and wrap the
  result in a v1 (8 byte) or v2 (12 byte) envelope with v2 trailing-zero
  truncation. These figures are a model, not a real dialect: no MAVLink
  dialect carries these messages, and a real one would make its own
  field and bound choices.
- Cycle counts come from `benchmark/encoding/fw_bench.c` on an Uno
  (ATmega328p at 16 MHz, avr-gcc 7.3.0, `-Os`), timed with Timer1 at one
  tick per cycle and read out over UART under simavr. The Mega shares the
  core and the clock, so its cycles match; its firmware build is tested.
  The test pins each payload length and a 400 cycles per byte tripwire.

## Size comparison

| Message | Profile | Packed | CDR | MAVLink v1 | MAVLink v2 |
| --- | --- | --- | --- | --- | --- |
| `std_msgs/msg/Float32` | wide | 4 | 8 | 12 | 16 |
| `geometry_msgs/msg/Twist` | wide | 48 | 52 | 56 | 60 |
| `geometry_msgs/msg/Twist` | narrow | 24 | 52 | 32 | 36 |
| `sensor_msgs/msg/Range` | wide | 35 | 44 | 53 | 52 |
| `sensor_msgs/msg/Range` | narrow | 35 | 44 | 53 | 52 |
| `sensor_msgs/msg/Imu` | wide | 309 | 324 | 328 | 329 |
| `sensor_msgs/msg/Imu` | narrow | 161 | 324 | 180 | 184 |
| `sensor_msgs/msg/JointState` | wide | 64 | 108 | 272 | 273 |
| `sensor_msgs/msg/JointState` | narrow | 48 | 108 | 200 | 201 |
| `builtin_interfaces/msg/Time` | wide | 8 | 12 | 16 | 17 |
| `geometry_msgs/msg/Vector3` | wide | 24 | 28 | 32 | 36 |
| `geometry_msgs/msg/Quaternion` | wide | 32 | 36 | 40 | 44 |
| `std_msgs/msg/Header` | wide | 13 | 21 | 32 | 33 |
| `std_msgs/msg/String` | wide | 6 | 14 | 24 | 17 |

MAVLink figures are a model, not a real dialect: fields reordered
largest first with the bundle's bounds, in a v1 or v2 envelope.

Packed is smallest on every row. CDR pays the 4 byte header plus padding
(Twist) and per-member length prefixes (JointState names). MAVLink-style
pays fixed bound-sized arrays whether the payload fills them or not, which
is why JointState (272 vs 64 packed) and the narrow rows diverge most.
Narrowing halves the float64 content and leaves CDR untouched, since CDR
has no narrow form.

## Encode and decode cycles on the MCU

Narrow payloads, Timer1 cycles on the Uno:

| Message | Encode | Decode | Bytes | Enc/byte | Dec/byte |
| --- | --- | --- | --- | --- | --- |
| `std_msgs/msg/Float32` | 1056 | 921 | 4 | 264 | 230 |
| `geometry_msgs/msg/Twist` | 5672 | 4659 | 24 | 236 | 194 |
| `sensor_msgs/msg/Range` | 7829 | 6279 | 35 | 224 | 179 |
| `sensor_msgs/msg/Imu` | 22979 | 15296 | 161 | 143 | 95 |
| `sensor_msgs/msg/JointState` | 9509 | 7322 | 48 | 198 | 153 |

Per-byte cost falls with message size: the interpreter's table walk is a
fixed cost that small payloads amortize poorly. Even the hottest row costs
under a third of the roughly 1390 cycles a byte occupies on the wire at
115200 baud, so encoding stays out of the link's way. The committed test
pins that ratio as a 400 cycles per byte tripwire rather than pinning
exact counts, which a toolchain update may shift.

## Narrow per field vs per topic

Float64 content by field group, wide vs narrow bytes:

| Message | Group | Wide | Narrow | Saves |
| --- | --- | --- | --- | --- |
| Imu | orientation | 32 | 16 | 16 |
| Imu | orientation_covariance | 72 | 36 | 36 |
| Imu | angular_velocity | 24 | 12 | 12 |
| Imu | angular_velocity_covariance | 72 | 36 | 36 |
| Imu | linear_acceleration | 24 | 12 | 12 |
| Imu | linear_acceleration_covariance | 72 | 36 | 36 |
| Imu | total | 309 | 161 | 148 |
| Twist | linear, angular | 48 | 24 | 24 |
| JointState | position, velocity | 32 | 16 | 16 |

Only Imu has a mixed-precision story: narrowing just its covariances
saves 108 of the 148 narrow bytes, while keeping the dynamics wide costs
40 bytes per message against full narrow (201 vs 161). Twist and
JointState are all dynamics, so per-field buys them nothing over
per-topic.

Recommendation: keep the per-topic narrow default and change nothing on
the wire or in code. Per-field narrowing would add per-field flags to the
descriptors, the configuration surface and the bridge conversion to save
40 bytes on one message type in its mixed case, while per-topic narrow
already captures the full saving everywhere. The mixed-precision figures
above stay on record for the maintainer's decision under `needs-decision`.

## Reproduce

Sizes and the pinned table:

```sh
python3 benchmark/encoding/sizes.py
python3 -m pytest tools/test_encoding_benchmark.py -q
```

Cycle counts (dev image, Uno firmware under simavr):

```sh
make pytest ARGS='tools/test_encoding_benchmark.py::test_encode_cycles_within_budget'
```
