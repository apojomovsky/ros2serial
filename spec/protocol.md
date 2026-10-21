# ros2serial protocol specification

Version 0.1 (draft) · protocol version 1

This document is normative. Every normative statement is a list item starting with an ID in brackets, such as `[FRM-1]`. Conformance vectors (`spec/vectors/`) and tests name the IDs they cover, so every rule here is checked by at least one test (`docs/01-testing.md`, section 3). IDs are never reused: a removed rule leaves its ID retired.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

This version covers the frame, the CRC, rCOBS, varints and the message encoding. System channels, the session state machines and time synchronization follow in the next version.

## 1. Conventions

- [GEN-1] All multi-byte integers on the wire are little-endian.
- [GEN-2] Sizes are in bytes. "Octet" and "byte" both mean 8 bits.
- [GEN-3] In this document and in the vectors, byte strings are written as lowercase hex byte pairs separated by single spaces, for example `10 02 01`.
- [GEN-4] The protocol version described here is 1. Implementations MUST carry it as `R2S_PROTOCOL_VERSION` and announce it in the handshake.

## 2. Frame

A frame carries one message on one topic between the host and the MCU.

```
frame = rCOBS( topic_id u8 | seq u8 | payload | crc16 u16 ) 0x00
```

- [FRM-1] A frame on the wire is the rCOBS encoding (section 4) of the decoded frame, followed by a single `0x00` delimiter.
- [FRM-2] The decoded frame is, in order: `topic_id` (1 byte), `seq` (1 byte), the payload (0 or more bytes), and `crc16` (2 bytes, little-endian).
- [FRM-3] `topic_id` values 0 to 15 are reserved for system channels; user topics use 16 to 255.
- [FRM-4] `seq` is a counter kept per `topic_id` and per direction. It starts at 0 after each handshake, increases by 1 for every frame sent on that topic in that direction, and wraps from 255 to 0.
- [FRM-5] A receiver MUST drop, and count, a decoded frame shorter than 4 bytes.
- [FRM-6] A receiver MUST drop, and count, a frame whose CRC check fails (section 3).
- [FRM-7] Each side has a maximum decoded frame size (`max_frame`, from `topic_id` to `crc16` inclusive). The MCU's is set at build time and announced in the handshake. The protocol allows up to 65535.
- [FRM-8] A sender MUST NOT send a decoded frame larger than the receiver's `max_frame`. A receiver MUST drop, and count, a frame larger than its own `max_frame`, discarding bytes until the next `0x00`.
- [FRM-9] After any error, a receiver resynchronizes at the next `0x00`: the bytes after it start a new frame. Two consecutive `0x00` bytes delimit an empty frame, which is ignored without being counted as an error.
- [FRM-10] A gap in `seq` on a user topic is counted as lost frames; it is not an error and does not drop the frame.

## 3. CRC

- [CRC-1] The frame check is CRC-16/MCRF4XX: width 16, polynomial `0x1021`, reflected input and output, initial value `0xFFFF`, no final XOR.
- [CRC-2] The CRC of the ASCII string `123456789` is `0x6F91`.
- [CRC-3] The sender computes the CRC over `topic_id`, `seq` and the payload, and appends it little-endian.
- [CRC-4] The receiver computes the CRC over the whole decoded frame, its two CRC bytes included, and accepts the frame only if the result is 0. (Because the CRC is reflected and appended little-endian, a correct frame always leaves this zero residue.)

## 4. rCOBS

Reverse Consistent Overhead Byte Stuffing removes every `0x00` from the frame, so `0x00` can delimit frames. Unlike COBS, each code byte follows the block it describes, so an encoder needs no lookahead and no buffer.

- [COB-1] Encoding: start with a run counter `n = 1`. For each input byte `b`: if `b` is `0x00`, emit `n` and set `n = 1`; otherwise emit `b`, increase `n` by 1, and if `n` reaches `0xFF`, emit `0xFF` and set `n = 1`. After the last input byte, emit `n`.
- [COB-2] The encoding of any input contains no `0x00` byte.
- [COB-3] The encoding is deterministic. In particular, 254 non-zero bytes at the end of the input produce a trailing empty group: the encoding ends in `... FF 01`.
- [COB-4] Decoding runs from the end of the encoded bytes (without the delimiter). Read the last byte as a code `c`; the `c - 1` bytes before it are data. Repeat with the bytes before those. Between two consecutive groups, the decoded output has a `0x00` after the earlier group, unless the earlier group's code is `0xFF`.
- [COB-5] A decoder MUST reject an encoding that contains a code of `0x00` or a code larger than the number of bytes left to decode.
- [COB-6] Decoding MAY write the output from the end of the buffer backwards, in place: the decoded frame ends where the encoded one ended.
- [COB-7] An encoded frame of `n` decoded bytes is at most `n + n / 254 + 1` bytes long (integer division), before the delimiter.

## 5. Varints

- [VAR-1] Lengths of strings and sequences are unsigned LEB128 varints: 7 bits per byte, least significant group first, the high bit set on every byte but the last.
- [VAR-2] A length is at most 65535. A decoder MUST reject a varint that encodes a larger value or uses more than 3 bytes.
- [VAR-3] A decoder MUST reject a varint truncated by the end of the payload.
- [VAR-4] An encoder MUST use the shortest encoding. A decoder MAY accept longer ones only if they still fit VAR-2.

## 6. Message encoding

A payload is the packed encoding of one message, field after field, with no padding, no alignment and no header.

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

- [ENC-1] Fields are encoded in declaration order, each according to the table above, with nothing between them.
- [ENC-2] Integers of every width are encoded little-endian; signed integers are two's complement.
- [ENC-3] `float32` fields are IEEE 754 binary32, little-endian.
- [ENC-4] `float64` fields are IEEE 754 binary64, little-endian, unless the topic is narrow (ENC-10).
- [ENC-5] `bool` is one byte, 0 or 1. A decoder MUST reject any other value.
- [ENC-6] `string` and `string<=N` are a varint byte length followed by the bytes, with no terminator. The bytes are passed through unchanged; UTF-8 validity is not checked on the wire.
- [ENC-7] Fixed arrays `T[N]` are N consecutive elements with no length prefix.
- [ENC-8] Unbounded and bounded sequences `T[]` and `T[<=N]` are a varint element count followed by the elements.
- [ENC-9] A nested message is encoded as its fields inline, recursively by the same rules.
- [ENC-10] A topic is narrow when its TOPIC entry has the narrow flag set. On a narrow topic every `float64` field travels as binary32; the host widens it on reception and narrows it, rounding to nearest, before sending.
- [ENC-11] An MCU build whose C `double` is 32 bits MUST set the narrow flag on every topic.
- [ENC-12] The wire layout does not depend on the bounds chosen for strings and sequences: only the actual lengths are encoded.
- [ENC-13] A sender MUST NOT truncate a string or sequence longer than its bound; it reports an error instead.
- [ENC-14] A receiver MUST drop a payload whose string or sequence length exceeds the bound it was built with, or that ends before the last field, or that has bytes left after the last field. It reports the topic and the field.
