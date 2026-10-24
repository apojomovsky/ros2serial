# ros2serial protocol specification

Version 0.3 (draft) · protocol version 1

This document is normative. Every normative statement is a list item starting with an ID in brackets, such as `[FRM-1]`. Conformance vectors (`spec/vectors/`) and tests name the IDs they cover, so every rule here is checked by at least one test (`docs/01-testing.md`, section 3). IDs are never reused: a removed rule keeps its list item, with its text replaced by "Retired.", so the numbering has no gaps.

A section whose heading ends in `(local)` specifies bytes that never appear on the wire, so tests, not conformance vectors, cover its rules.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119. Prose outside the ID items, tables included, is informative.

This version covers the frame, the frame check, rCOBS, varints, the message encoding, the codec descriptors, the system channels, the handshake, the session state machines, the error counters and time synchronization. The parameters and reliable-channel channels follow in a later version.

## 1. Conventions

Sizes are in bytes, and a byte is 8 bits. In this document and in the vectors, byte strings are written as lowercase hex byte pairs separated by single spaces, for example `10 02 01`.

- [GEN-1] All multi-byte integers on the wire are little-endian.
- [GEN-2] The protocol version described here is 1. Both sides announce it in the handshake.

## 2. Frame

A frame carries one message on one topic between the host and the MCU.

```
frame = rCOBS( topic_id u8 | seq u8 | payload | crc16 u16 ) 0x00
```

- [FRM-1] A frame on the wire is the rCOBS encoding (section 4) of the decoded frame, followed by a single `0x00` delimiter.
- [FRM-2] The decoded frame is, in order: `topic_id` (1 byte), `seq` (1 byte), the payload (0 or more bytes), and `crc16` (2 bytes, little-endian).
- [FRM-3] `topic_id` values 0 to 15 are reserved for system channels; user topics use 16 to 255.
- [FRM-4] `seq` is a counter kept per `topic_id` and per direction. It is 0 in the first frame on that topic in that direction after each handshake.
- [FRM-5] Each further frame on that topic in that direction carries the previous `seq` plus 1, modulo 256.
- [FRM-6] A receiver MUST drop a frame whose rCOBS decoding fails ([COB-5], [COB-6]), and count it as a dropped frame (section 7.4).
- [FRM-7] A receiver MUST drop a decoded frame shorter than 4 bytes, and count it as a dropped frame. The encoded frame `01` (0 decoded bytes) is such a frame.
- [FRM-8] A receiver MUST drop a frame whose CRC check fails ([CHK-4]), and count it as a CRC error and as a dropped frame.
- [FRM-9] The MCU's maximum decoded frame size, `max_frame` (from `topic_id` to `crc16` inclusive), is set at build time and announced in the handshake. It is at most 65277, so that the encoded frame and its delimiter fit in 65535 bytes ([COB-8]).
- [FRM-10] The host's `max_frame` is 65277 and is not announced.
- [FRM-11] A sender MUST NOT send a decoded frame larger than the receiver's `max_frame`.
- [FRM-12] A receiver MUST accept every frame whose decoded size is at most its `max_frame`, so it buffers up to the [COB-8] bound of encoded bytes. Once the encoded bytes of a frame exceed that bound, it MUST drop the frame and count it as a receive overflow and as a dropped frame, discarding bytes until the next `0x00`.
- [FRM-13] After any error, a receiver resynchronizes at the next `0x00`: the bytes after it start a new frame. A receiver starts as if it had just received a `0x00`.
- [FRM-14] Two consecutive `0x00` bytes delimit an empty encoded frame (no bytes at all), which is ignored without being counted. A sender MAY send a `0x00` before a frame, for example to flush a partial frame left on the line.
- [FRM-15] On a user topic, a receiver counts `(seq - expected) mod 256` lost frames, where `expected` is the previous `seq` plus 1, or 0 for the first frame after a handshake. A gap is not an error and does not drop the frame.

## 3. Frame check

- [CHK-1] The frame check is CRC-16/MCRF4XX: width 16, polynomial `0x1021`, reflected input and output, initial value `0xFFFF`, no final XOR.
- [CHK-2] The CRC of the ASCII string `123456789` is `0x6F91`.
- [CHK-3] The sender computes the CRC over `topic_id`, `seq` and the payload, and appends it little-endian.
- [CHK-4] The receiver computes the CRC over the whole decoded frame, its two CRC bytes included, and accepts the frame only if the result is 0.

Because the CRC is reflected and appended little-endian, a correct frame always leaves this zero residue, so a receiver needs no separate comparison.

## 4. rCOBS

Reverse Consistent Overhead Byte Stuffing removes every `0x00` from the frame, so `0x00` can delimit frames. Unlike COBS, each code byte follows the block it describes, so an encoder needs no lookahead and no buffer. A decoder can write its output from the end of the buffer backwards, in place: the decoded frame ends where the encoded one ended.

- [COB-1] Encoding: start with a run counter `n = 1`. For each input byte `b`: if `b` is `0x00`, emit `n` and set `n = 1`; otherwise emit `b`, increase `n` by 1, and if `n` reaches `0xFF`, emit `0xFF` and set `n = 1`. After the last input byte, emit `n`.
- [COB-2] The encoding of any input contains no `0x00` byte.
- [COB-3] The encoding is deterministic. In particular, 254 non-zero bytes at the end of the input produce a trailing empty group: the encoding ends in `... FF 01`.
- [COB-4] Decoding runs from the end of the encoded bytes (without the delimiter). Read the last byte as a code `c`; the `c - 1` bytes before it are that group's data. Repeat with the bytes before those, until none are left.
- [COB-5] A decoder MUST reject an encoding that contains a `0x00` byte, or a code `c` with fewer than `c - 1` bytes before it.
- [COB-6] A decoder MUST reject an encoding whose last byte is `0xFF`. ([COB-1] never produces one, and accepting it would give a second encoding of the same input.)
- [COB-7] In the decoded output, each group's data is followed by a `0x00` when the next group towards the end of the encoding exists and the group's own code is not `0xFF`. The group nearest the end is never followed by a `0x00`.
When an encoding breaks several rules, a decoder names the first one it meets: [COB-6] before any code is read, then [COB-5] group by group from the end. Conformance vectors only use encodings that break a single rule.

- [COB-8] An encoded frame of `n` decoded bytes is at most `n + n / 254 + 1` bytes long (integer division), before the delimiter. The bound is reached when the input has no `0x00`.

## 5. Varints

- [VAR-1] Lengths of strings and sequences are unsigned LEB128 varints: 7 bits per byte, least significant group first, the high bit set on every byte but the last.
- [VAR-2] A length is at most 65535, so a varint has at most 3 bytes. A decoder MUST reject a varint that encodes a larger value, or whose third byte has the high bit set.
- [VAR-3] A decoder MUST reject a varint truncated by the end of the payload.
- [VAR-4] An encoder MUST use the shortest encoding, and a decoder MUST reject any other (for example `80 00` for 0). A valid varint is therefore 1 to 3 bytes long.

A decoder applies these rules in the order it reads the bytes: a varint is rejected by [VAR-2] as soon as its third byte has the high bit set, by [VAR-3] when the payload ends first, and by [VAR-4] once its last byte is read. Conformance vectors only use encodings that break a single rule.

## 6. Message encoding

A payload is the packed encoding of one message, field after field, with no padding, no alignment and no header. The table summarizes the rules below.

| `.msg` type | Wire |
| --- | --- |
| `bool`, `byte`, `char`, `int8`, `uint8` | 1 byte |
| `int16` to `int64`, `uint16` to `uint64` | Fixed width, little-endian, two's complement for signed types |
| `float32` | IEEE 754 binary32, little-endian |
| `float64` | IEEE 754 binary64, little-endian; binary32 when the topic is narrow |
| `string`, `string<=N` | Varint length, then the bytes, no terminator |
| `T[N]` | N elements, no length prefix |
| `T[]`, `T[<=N]` | Varint length, then the elements |
| Nested message | Its fields inline, in declaration order |

- [ENC-1] Fields are encoded in declaration order, each according to its type's rule below, with nothing between them.
- [ENC-2] Integers of every width are encoded little-endian; signed integers are two's complement.
- [ENC-3] `float32` fields are IEEE 754 binary32, little-endian.
- [ENC-4] `float64` fields are IEEE 754 binary64, little-endian, unless the topic is narrow ([ENC-10]).
- [ENC-5] A sender encodes `bool` as one byte, 0 for false and 1 for true.
- [ENC-6] A receiver MUST drop a payload with a `bool` byte other than 0 or 1.
- [ENC-7] `string` and `string<=N` are a varint byte length followed by the bytes, with no terminator. The length may be 0. The bytes are passed through unchanged; UTF-8 validity is not checked on the wire.
- [ENC-8] Fixed arrays `T[N]` are N consecutive elements with no length prefix.
- [ENC-9] Unbounded and bounded sequences `T[]` and `T[<=N]` are a varint element count, which may be 0, followed by the elements.
- [ENC-10] A topic is narrow when its topic announcement has the narrow flag set.
- [ENC-11] On a narrow topic every `float64` field travels as IEEE 754 binary32, little-endian. A side holding the value as binary64 converts it with IEEE 754 roundTiesToEven before sending: a finite value too large for binary32 becomes an infinity of the same sign, a finite value below the smallest normal becomes the smallest normal, a subnormal or a zero of the same sign, and a NaN stays a NaN. The receiving host widens the value exactly.
- [ENC-12] An MCU build whose C `double` is 32 bits MUST set the narrow flag on every topic.
- [ENC-13] The wire layout does not depend on the bounds chosen for strings and sequences: only the actual lengths are encoded. A message encodes to the same bytes whatever bounds the two sides were built with, as long as it fits them.
- [ENC-14] A string MUST NOT contain a `0x00` byte, because the MCU holds strings NUL-terminated. A sender MUST NOT send one, and a receiver MUST drop a payload with one.
- [ENC-15] The MCU MUST NOT truncate a string or sequence longer than its bound: the publish fails with a bounds error and sends nothing. The host has no bounds.
- [ENC-16] The MCU MUST drop, and report, a payload whose string or sequence length exceeds the bound it was built with.
- [ENC-17] A receiver MUST drop, and count, a payload that ends before its last field.
- [ENC-18] A receiver MUST drop, and count, a payload with bytes left after its last field.
- [ENC-19] The table above is exhaustive. `wstring` and `wchar` have no wire encoding, and the generator rejects a message that uses them.

## 7. System channels

A system channel carries one message on a reserved `topic_id` (section 2, [FRM-3]). The payload of a system channel on ID 0 or ID 1 begins with an opcode byte; the other system channels have no opcode.

- [SYS-1] The system channels are: ID 0 control, ID 1 time, ID 2 log, ID 3 parameters, ID 4 heartbeat, ID 5 error report, ID 6 reliable-channel ack. IDs 7 to 15 are reserved and MUST NOT be used.
- [SYS-2] A frame on a system channel uses the same framing as a user topic (section 2), so it has a `seq` counter and a CRC.
- [SYS-3] The parameters and reliable-channel ack channels are specified in a later version of this document. Until then, a receiver MUST ignore a frame on them.
- [SYS-4] A receiver MUST ignore a frame on a reserved channel (ID 7 to 15) and MUST NOT count it as an error.
- [SYS-5] A receiver that refuses a frame drops it and takes no other action on it. The MCU also counts it as a dropped frame (section 7.4), which is the only side that keeps that counter.

### 7.1 Control channel (ID 0)

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | HELLO_REQ | host to MCU | `protocol_version u8` |
| 0x02 | HELLO | MCU to host | `protocol_version u8, handshake_mode u8, boot_id u32, max_frame u16, rx_capacity u16, tick_hz u32, features u8, topic_count u8`, then `node_name string` (full) or `node_name_hash u32` (compact) |
| 0x03 | TOPIC_REQ | host to MCU | `index u8` |
| 0x04 | TOPIC | MCU to host | `index u8, topic_id u8, flags u8, type_hash[8]`, then `type_name string, topic_name string` (full) or `topic_name_hash u32` (compact) |
| 0x05 | TOPIC_STATUS | host to MCU | `topic_id u8, status u8` |
| 0x06 | READY | host to MCU | none |

- [CTL-1] The handshake mode is 0 for full and 1 for compact.
- [CTL-2] The `features` byte has bit 0 for time, bit 1 for log, bit 2 for parameters, bit 3 for the reliable channel, bit 4 for "`double` is 32 bits" and bit 5 for runtime topic registration. Bits 4 and 5 are informational. Every other bit is reserved and MUST be 0 in both directions.
- [CTL-3] A receiver MUST refuse ([SYS-5]) a frame whose `features` byte has a reserved bit set.
- [CTL-4] The `flags` byte has bit 0 for the direction (0 when the MCU publishes the topic, 1 when it subscribes), bit 1 for narrow `float64`, bit 2 for keep-latest and bit 3 for the reliable channel. Every other bit is reserved and MUST be 0.
- [CTL-5] The flags are the truth for the wire. A receiver MUST use the announced flags rather than its own build to decide whether a topic is narrow.
- [CTL-6] Retired. It said what [ENC-12] says, and a rule is never renumbered.
- [CTL-7] `rx_capacity` is the size of the MCU's receive frame buffer in bytes.
- [CTL-8] `tick_hz` MUST be `1000 × k` with `k` a divisor of 10^6, so that a tick rate converts to nanoseconds with integer math alone.
- [CTL-9] A receiver MUST refuse ([SYS-5]) a `tick_hz` that is not of that form.
- [CTL-10] `max_frame` is the largest decoded frame the MCU accepts, and MUST NOT be larger than the limit of section 2 ([FRM-9]).
- [CTL-11] A name hash in compact mode is FNV-1a 32-bit over the UTF-8 bytes of the name.
- [CTL-12] The `type_hash` field is the first 8 bytes of the type's RIHS01 hash. A receiver MUST compare only the 8 bytes on the wire, and a mismatch of any of them rejects the topic.
- [CTL-13] A `TOPIC_STATUS` status is 0 for ok, 1 for unknown type, 2 for hash mismatch and 3 for name conflict. Every other value is reserved and MUST be 0.
- [CTL-14] A receiver MUST refuse ([SYS-5]) a `TOPIC_STATUS` whose status is not one of those four.
- [CTL-15] The topics of a handshake are requested and sent one at a time, so the MCU never needs a buffer larger than one TOPIC frame.
- [CTL-16] The payload of each control opcode is the one the table above gives, in that order and with nothing between: the opcode byte, then the fields it lists, and a string as its varint length and bytes (section 5).

### 7.2 Time channel (ID 1)

| Opcode | Name | Direction | Payload after the opcode |
| --- | --- | --- | --- |
| 0x01 | PING | host to MCU | `ping_id u8` |
| 0x02 | PONG | MCU to host | `ping_id u8, tick u32` |
| 0x03 | CORRECTION | host to MCU | `tick0 u32, sec i32, nanosec u32, drift_ppm i16` |

- [TIM-1] The `tick` of a PONG is the tick at which the PING's delimiter arrived.
- [TIM-2] A PONG MUST carry the `ping_id` of the PING it answers.
- [TIM-3] `tick0` is the tick at which the correction's time was measured, so that the MCU can compute the elapsed ticks.
- [TIM-4] `drift_ppm` is the number of nanoseconds of ROS time to add per nominal millisecond of MCU time, and MUST be within ±20000.
- [TIM-5] A receiver MUST refuse ([SYS-5]) a CORRECTION whose `drift_ppm` is outside ±20000.

The MCU computes its time from the correction in 32-bit integers alone. With `k = tick_hz / 1000` ticks per millisecond and `p = 10^6 / k` nanoseconds per tick, and `e = tick - tick0` computed as an unsigned 32-bit difference, so that it wraps correctly:

```
ms  = e / k;  r = e % k
if ms > R2S_TIME_MAX_ANCHOR_MS: time is unsynced
sec = sec0 + ms / 1000
ns  = nanosec0 + (ms % 1000) * 1000000 + r * p
normalize ns into [0, 10^9) by carrying into sec
ns  = ns + ms * drift_ppm
normalize again
```

- [TIM-6] The MCU MUST compute its time with the integer arithmetic above and MUST NOT use floating point.
- [TIM-7] `R2S_TIME_MAX_ANCHOR_MS` defaults to 10000. A correction older than that makes the MCU's time unsynced, and the MCU MUST report it as unsynced until a new correction arrives.
- [TIM-8] The intermediate nanosecond value before the drift is added MUST fit in a 32-bit unsigned integer, and the drift term MUST fit in a 32-bit signed integer.
- [TIM-9] The MCU MUST normalize the nanosecond value into [0, 10^9) by carrying into the seconds, before and after the drift is added.
- [TIM-10] The payload of each time opcode is the one the table above gives, in that order and with nothing between: the opcode byte, then the fields it lists.
- [TIM-11] The `nanosec` of a CORRECTION MUST be less than 1000000000, and a receiver MUST refuse ([SYS-5]) a CORRECTION whose `nanosec` is not.

### 7.3 Log channel (ID 2)

The MCU sends on this channel; the host does not.

- [LOG-1] The payload is `level u8, text string, value_kind u8`, then the value when `value_kind` is not 0.
- [LOG-2] A level is 10 for debug, 20 for info, 30 for warn, 40 for error and 50 for fatal, the ROS severities.
- [LOG-3] A value kind is 0 for none, 1 for `u16`, 2 for `i16`, 3 for `u32`, 4 for `i32` and 5 for `f32`. Every other value is reserved and MUST be 0.
- [LOG-4] A receiver MUST refuse ([SYS-5]) a log frame whose value kind is not one of those six.

### 7.4 Heartbeat channel (ID 4)

- [HB-1] A heartbeat from the host has an empty payload.
- [HB-2] A heartbeat from the MCU carries `rx_frames u16, crc_errors u16, rx_overflows u16, dropped_frames u16`.
- [HB-3] The four counters are wrapping 16-bit counters, and each counts the frames of its own kind since the MCU booted. A frame that two rules count, such as one that fails the CRC check, counts in both of them.
- [HB-4] `rx_frames` counts the frames the MCU received and accepted.
- [HB-5] `crc_errors` counts the frames the MCU dropped because their CRC check failed ([FRM-8]).
- [HB-6] `rx_overflows` counts the frames the MCU dropped because their encoded bytes exceeded its buffer ([FRM-12]).
- [HB-7] `dropped_frames` counts every frame the MCU dropped, whichever rule dropped it ([FRM-6], [FRM-7], [FRM-8], [FRM-12]). It is therefore the total of the drops, and is not the sum of the other three.

### 7.5 Error report channel (ID 5)

The MCU sends on this channel; the host does not.

- [ERR-1] The payload is `code u8, topic_id u8, count u16, field u8`.
- [ERR-2] A code is 1 for a bound violation on decode, 2 for an unknown topic ID, 3 for a receive overflow, 4 for a frame too large, 5 for a publish while not connected and 6 for a busy transmitter.
- [ERR-3] A publish while not connected is reported once per connection, and a busy transmitter at most once per second.
- [ERR-4] The `field` is the index of the offending field in depth-first declaration order over the type's field definitions, where the elements of an array are not counted separately, or 0xFF when the rule does not name a field.
- [ERR-5] The host computes the same field order with introspection, so that it can log the path of the offending field.
- [ERR-6] The `count` is the number of times the error occurred since the last error report of that code, and it wraps at 65535. A code that [ERR-3] rate-limits is counted the same way and reported no more often than that.

## 8. Session

The MCU and the host each run a state machine. A handshake brings them from a fresh start to a link that carries user traffic.

### 8.1 The MCU session

```
BOOT --init, send HELLO--> ANNOUNCING --READY--> CONNECTED
^                          |                     |
|                          +- every 1 s with no host traffic: send HELLO
+-- link timeout, or HELLO_REQ received
```

- [SES-1] The MCU starts in BOOT, sends HELLO, and moves to ANNOUNCING.
- [SES-2] In ANNOUNCING the MCU answers a HELLO_REQ with HELLO and a TOPIC_REQ with TOPIC, and stores the TOPIC_STATUS of each topic.
- [SES-3] In ANNOUNCING, a host frame that is neither HELLO_REQ nor TOPIC_REQ is answered with HELLO, at most once per 100 ms, and is otherwise ignored. A user topic frame is ignored in the same way.
- [SES-4] In ANNOUNCING, a publish fails with the not-connected error and sends nothing.
- [SES-5] The MCU sends HELLO every second while it is in ANNOUNCING and the host has sent nothing.
- [SES-6] The MCU moves from ANNOUNCING to CONNECTED when the host sends READY.
- [SES-7] The MCU returns to ANNOUNCING on a link timeout or on a HELLO_REQ.
- [SES-8] A link timeout is no valid frame from the host for 2000 ms. The interval is a build-time setting.
- [SES-9] In CONNECTED the MCU sends a heartbeat every 500 ms, and a publish works for the topics whose status is ok.
- [SES-10] The `boot_id` comes from the port, once per boot, so that a host can tell a reset from a reconnect.
- [SES-11] The `seq` counter of every topic restarts at 0 after each handshake (section 2, [FRM-4]).

### 8.2 The host session

```
DISCONNECTED --port open--> BOOT_WAIT --HELLO or boot_wait_ms--> HANDSHAKING --all topics resolved, READY sent--> CONNECTED
^                                                                ^                                                |
+-- port error                                                   +--- link timeout or reset detected              +
```

- [SES-12] In DISCONNECTED the host tries to open the port every 1000 ms.
- [SES-13] After it opens the port, the host waits up to 2500 ms for an unsolicited HELLO before it sends anything, so that a board reset by DTR is not talking to its bootloader.
- [SES-14] In HANDSHAKING the host sends HELLO_REQ every 500 ms until a HELLO arrives.
- [SES-15] The host refuses a HELLO whose protocol version it does not support, and logs an error naming both versions.
- [SES-16] For each topic index, the host sends TOPIC_REQ with a 200 ms timeout and at most 3 retries, resolves the type support, checks the hash, sends TOPIC_STATUS, and creates or reuses the publisher or subscription.
- [SES-17] The host sends READY when every topic is resolved.
- [SES-18] In CONNECTED the host sends a heartbeat every 500 ms and forwards user traffic.
- [SES-19] The host detects a reset when a HELLO arrives while it is connected, or when a HELLO carries a different `boot_id`.
- [SES-20] On a re-handshake the host keeps its publishers and subscriptions when the new topic table is identical to the previous one, and destroys and recreates them otherwise.
- [SES-21] The host returns to DISCONNECTED on a port error, and to HANDSHAKING on a link timeout or a reset.

### 8.3 Time synchronization on the host

- [SES-22] Right after READY, the host sends 8 PINGs 50 ms apart.
- [SES-23] After that, the host sends one PING every 1000 ms for the first 10 s, and then every `time_sync.period_ms`, which defaults to 5000.
- [SES-24] The estimator keeps the 16 most recent samples, uses the ones with the lowest round trip for the offset, and fits a line for the drift.
- [SES-25] The host sends a CORRECTION after the initial burst, whenever the estimate moves by more than 100 µs, and at least every 5 s.

## 9. Codec descriptors (local)

One interpreter in the MCU encodes and decodes every message type from constant tables in flash: a type record for each message type, and a field entry for each field. The tables are built with the firmware and never cross the link, so this section fixes a build artifact rather than a frame. It is normative because the generator and a handwritten table must agree on it byte for byte, and because a table that breaks these rules makes the interpreter read outside a struct.

The interpreter reads a table one byte at a time through `R2S_FLASH_READ_BYTE()`, so the layout below holds on every target, whatever its byte order and alignment rules.

### 9.1 The two tables

A type record says where a type's fields are, and a field entry says how one field is stored. Both are addressed by index: a type record by its position in the type table, a field entry by its position in the field table.

| Type record, 5 bytes | Width | Meaning |
| --- | --- | --- |
| `size` | 2 | `sizeof` of the type's C struct |
| `first_field` | 2 | index of the type's first field entry |
| `field_count` | 1 | number of the type's field entries |

| Field entry, 8 bytes | Width | Meaning |
| --- | --- | --- |
| `kind` | 1 | the field's type, section 9.2 |
| `mode` | 1 | scalar, fixed array or bounded array, section 9.2 |
| `offset` | 2 | byte offset of the field's storage in the type's struct |
| `count` | 2 | array length or bound, 0 for a scalar |
| `aux` | 2 | string bound or type index, 0 otherwise |

- [DSC-1] A descriptor table is two constant byte arrays: the type records and the field entries. Nothing else describes a message type to the interpreter.
- [DSC-2] A type record is 5 bytes, in this order: `size` as a 2-byte integer, `first_field` as a 2-byte integer, `field_count` as a 1-byte integer.
- [DSC-3] A field entry is 8 bytes, in this order: `kind` (1 byte), `mode` (1 byte), `offset` as a 2-byte integer, `count` as a 2-byte integer, `aux` as a 2-byte integer.
- [DSC-4] Every multi-byte integer of a table is little-endian, whatever the target's byte order, because the interpreter assembles each one from bytes; the table keeps the wire's byte order, so one generator order serves every target.
- [DSC-5] `size` and `offset` are those of the C struct the same build emits, so a table describes its own build's structs and never another build's.
- [DSC-6] `first_field` and `field_count` address the entries of one type: its fields are the entries from `first_field` to `first_field + field_count - 1`.
- [DSC-7] `field_count` is 0 for a message with no fields, which then has no entries of its own.
- [DSC-8] The entries of one type are contiguous and follow the declaration order of its fields, which is the order [ENC-1] encodes them in.
- [DSC-9] The two tables have no gaps: the first type record has `first_field` 0, and every further record starts at the previous record's `first_field` plus its `field_count`.
- [DSC-10] A message type appears at most once in a table, however many topics and fields use it, and a topic names its type by the index of its record.
- [DSC-11] The type records are numbered by a depth-first walk of the configured topics in configuration order: at each field in declaration order, a nested type is numbered before the type that contains it, and a type that already has an index keeps it.
- [DSC-12] A nested message field's `aux` is the index of its type's record and is less than the index of the record that names it, so a table has no cycle and the interpreter needs no list of visited types.

### 9.2 Kinds, modes and storage

| `kind` | `.msg` type | C type | Size in the struct |
| --- | --- | --- | --- |
| 0 | `bool` | `uint8_t` | 1 |
| 1 | `byte`, `char`, `uint8` | `uint8_t` | 1 |
| 2 | `int8` | `int8_t` | 1 |
| 3 | `uint16` | `uint16_t` | 2 |
| 4 | `int16` | `int16_t` | 2 |
| 5 | `uint32` | `uint32_t` | 4 |
| 6 | `int32` | `int32_t` | 4 |
| 7 | `uint64` | `uint64_t`, or `r2s_u64_t` with `R2S_NO_INT64` | 8 |
| 8 | `int64` | `int64_t`, or `r2s_i64_t` with `R2S_NO_INT64` | 8 |
| 9 | `float32` | `float` | 4 |
| 10 | `float64` | `double` | `sizeof(double)` |
| 11 | `string<=N` | `char name[N + 1]` | `aux + 1` |
| 12 | Nested message | its struct | the nested record's `size` |

- [DSC-13] `kind` is one of the values in the table above, and every other value is reserved.
- [DSC-14] `mode` is 0 for a scalar, 1 for a fixed array and 2 for a bounded array. Bit 7 (0x80) is set only with mode 2, where it says that the array's count member is 2 bytes wide instead of 1. Bits 1 to 6 are reserved and MUST be 0.
- [DSC-15] `count` is 0 for a scalar, the number of elements of a fixed array, and the bound N of a bounded array `T[<=N]`.
- [DSC-16] `aux` is the bound N of a string, the type index of a nested message, and 0 for every other kind.
- [DSC-17] `offset` is the start of the field's storage: the value of a scalar, the first element of an array, the first character of a string, and the struct of a nested message.
- [DSC-18] The size of one element is the size the table above gives, `aux + 1` for a string, and the nested record's `size` for a nested message.
- [DSC-19] A scalar's storage is one element at `offset`. A fixed array's storage is `count` elements from `offset`. A bounded array's storage is `count` elements from `offset`, then its count member. A string's storage is `aux + 1` bytes from `offset`.
- [DSC-20] A bounded array's count member is 1 byte, or 2 bytes when bit 7 of `mode` is set, and starts as far past the last element as its width requires: its distance from the field's `offset` is the size of all the elements rounded up to a multiple of the member's width. The struct a build emits holds the elements first and puts the count member there, adding the padding that a target with weaker alignment would leave out, so the table describes the same layout on every target.
- [DSC-21] A field's storage lies within its type: `offset` plus the storage of [DSC-19] is at most the record's `size`, and so is the end of the count member of [DSC-20]; every sum there is computed in at least 32 bits so that it cannot wrap.
- [DSC-22] An unbounded sequence `T[]` and an unbounded `string` have no descriptor form: the configuration gives each of them a bound, and the field is then a bounded array or a bounded string, whose wire form is [ENC-9]'s and [ENC-7]'s. `wstring` and `wchar` have no kind at all ([ENC-19]). A message with more than 255 fields, a type whose struct exceeds 65535 bytes, and a table with more than 65535 field entries have no descriptor form either, and the generator MUST reject each of them rather than truncate a count.
- [DSC-23] A `bool` field is one byte holding 0 or 1, which is what [ENC-5] sends and what [ENC-6] accepts.
- [DSC-24] Kinds 7 and 8 occupy 8 bytes with and without `R2S_NO_INT64`, so the interpreter copies their bytes and never computes with them.

### 9.3 The narrow flag

The narrow flag belongs to a topic, not to a type ([CTL-4] bit 1, [ENC-10]), so one table serves its type on a narrow and on a wide topic.

- [DSC-25] A table carries no narrow flag. The interpreter is told whether the topic it encodes or decodes for is narrow, and applies [ENC-11] when it is.
- [DSC-26] A `float64` field occupies `sizeof(double)` bytes in the struct, and 4 bytes on the wire on a narrow topic and 8 on any other ([ENC-4], [ENC-11]).
- [DSC-27] On a build whose `double` is 32 bits (feature bit 4, [CTL-2]) the storage of a `float64` field is the 4 bytes of binary32, and [ENC-12] has such a build set narrow on every topic, so narrowing there is a copy of 4 bytes.

### 9.4 Encoding and decoding

The interpreter walks a type's entries from `first_field` in order. The field order of a payload is therefore the order of the field entries, which is the declaration order of [ENC-1].

- [DSC-28] A type is encoded by visiting its entries in order and decoded in the same order, entering the record of a nested message where a `NEST` field is met.
- [DSC-29] A scalar is encoded and decoded as the rule of section 6 for its kind gives, at its `offset` and with its element size.
- [DSC-30] A fixed array is `count` elements with no length on the wire ([ENC-8]).
- [DSC-31] A bounded array is a varint of its count member ([ENC-9]) followed by its elements: an encode sends the count the struct holds, and a decode reads the count, drops a payload whose count exceeds the bound ([ENC-16]), stores it in the count member and reads that many elements.
- [DSC-32] A string is the bytes before its terminating `0x00`, with no terminator on the wire ([ENC-7]). A string with no terminator among its `aux + 1` bytes fails the publish with a bounds error ([ENC-15]). A decode drops a payload whose length exceeds `aux` ([ENC-16]), and writes the bytes and a terminator.
- [DSC-33] A nested message is its own type's fields inline ([ENC-1]), in the `size` bytes of its record.
- [DSC-34] A value is read from and written to the struct in the target's byte order and appears on the wire little-endian ([GEN-1]), so the interpreter swaps the bytes of every multi-byte kind on a big-endian target.
- [DSC-35] A publish whose value does not fit a field's bound fails with `R2S_ERR_BOUNDS` and sends nothing: the interpreter never truncates a string or an array ([ENC-15]).
- [DSC-36] A decode that breaks a rule of section 6 or of this section drops the payload, and the rule that broke is the reason ([ENC-6], [ENC-16], [ENC-17], [ENC-18]).
- [DSC-37] The field an error names is [ERR-4]'s index: the number of field entries the interpreter visited before the offending one, counting the entries of the nested messages it entered and counting an array once, however many elements it has.

### 9.5 Nesting

- [DSC-38] The depth of a type is 1 when it has no `NEST` field, and otherwise 1 more than the greatest depth among the types its `NEST` fields name. An array of a nested message adds no level, because its elements are a loop.
- [DSC-39] `R2S_MAX_NEST` is the greatest depth among the types a build uses, and at least 1.
- [DSC-40] The interpreter keeps one frame per message level on an explicit stack of `R2S_MAX_NEST` frames and never recurses, so the RAM it spends on nesting does not depend on the payload.

### 9.6 Validation

A loader is the code that checks a table: the generator before it emits one, and the tests of a handwritten table. The interpreter does not check its own table at run time, because a table is compile-time data and cannot change while the firmware runs.

- [DSC-41] A table is valid when every rule of this section holds, and a loader MUST reject a table that breaks any of them, naming the rule it broke.
- [DSC-42] A loader MUST reject a `kind` that is not one of [DSC-13]'s.
- [DSC-43] A loader MUST reject a `mode` with a reserved bit set, or with bit 7 set on a mode other than 2.
- [DSC-44] A loader MUST reject a scalar whose `count` is not 0, an array whose `count` is 0, and a bounded array whose bit 7 disagrees with whether `count` is above 255.
- [DSC-45] A loader MUST reject a non-zero `aux` on a kind other than a string and a nested message.
- [DSC-46] A loader MUST reject a nested message field whose `aux` is not the index of a record of the table, or is not less than the index of the record that names it.
- [DSC-47] A loader MUST reject a record whose `size` is 0, an entry outside the field table, and a record whose `first_field` does not follow the previous record as [DSC-9] requires.
- [DSC-48] A loader MUST reject a field whose storage does not lie within its type, computing the check as [DSC-21] requires.
- [DSC-49] A loader MUST reject a table whose depth exceeds `R2S_MAX_NEST` ([DSC-38], [DSC-39]).
