# ros2serial protocol specification

Version 0.1 (draft) · protocol version 1

This document is normative. Every normative statement is a list item starting with an ID in brackets, such as `[FRM-1]`. Conformance vectors (`spec/vectors/`) and tests name the IDs they cover, so every rule here is checked by at least one test (`docs/01-testing.md`, section 3). IDs are never reused: a removed rule keeps its list item, with its text replaced by "Retired.", so the numbering has no gaps.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119. Prose outside the ID items, tables included, is informative.

This version covers the frame, the frame check, rCOBS, varints and the message encoding. System channels, the handshake, the session state machines, the error counters and time synchronization follow in the next version (ticket S2). Where a rule below says a frame or payload is "counted" or "reported", which counter or report it is belongs to that version.

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
- [FRM-6] A receiver MUST drop, and count, a frame whose rCOBS decoding fails ([COB-5], [COB-6]).
- [FRM-7] A receiver MUST drop, and count, a decoded frame shorter than 4 bytes. The encoded frame `01` (0 decoded bytes) is such a frame.
- [FRM-8] A receiver MUST drop, and count, a frame whose CRC check fails ([CHK-4]).
- [FRM-9] The MCU's maximum decoded frame size, `max_frame` (from `topic_id` to `crc16` inclusive), is set at build time and announced in the handshake. It is at most 65277, so that the encoded frame and its delimiter fit in 65535 bytes ([COB-8]).
- [FRM-10] The host's `max_frame` is 65277 and is not announced.
- [FRM-11] A sender MUST NOT send a decoded frame larger than the receiver's `max_frame`.
- [FRM-12] A receiver MUST accept every frame whose decoded size is at most its `max_frame`, so it buffers up to the [COB-8] bound of encoded bytes. Once the encoded bytes of a frame exceed that bound, it MUST drop and count the frame, discarding bytes until the next `0x00`.
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
- [ENC-11] On a narrow topic every `float64` field travels as IEEE 754 binary32, little-endian. A side holding the value as binary64 converts it with IEEE 754 roundTiesToEven before sending: a finite value too large for binary32 becomes an infinity of the same sign, and a NaN stays a NaN. The receiving host widens the value exactly.
- [ENC-12] An MCU build whose C `double` is 32 bits MUST set the narrow flag on every topic.
- [ENC-13] The wire layout does not depend on the bounds chosen for strings and sequences: only the actual lengths are encoded. A message encodes to the same bytes whatever bounds the two sides were built with, as long as it fits them.
- [ENC-14] A string MUST NOT contain a `0x00` byte, because the MCU holds strings NUL-terminated. A sender MUST NOT send one, and a receiver MUST drop a payload with one.
- [ENC-15] The MCU MUST NOT truncate a string or sequence longer than its bound: the publish fails with a bounds error and sends nothing. The host has no bounds.
- [ENC-16] The MCU MUST drop, and report, a payload whose string or sequence length exceeds the bound it was built with.
- [ENC-17] A receiver MUST drop, and count, a payload that ends before its last field.
- [ENC-18] A receiver MUST drop, and count, a payload with bytes left after its last field.
- [ENC-19] The table above is exhaustive. `wstring` and `wchar` have no wire encoding, and the generator rejects a message that uses them.
