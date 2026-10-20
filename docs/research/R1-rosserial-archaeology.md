# R1: rosserial archaeology

Oct 20, 2026 · research ticket R1 in `proposal-v3.md`

**Sources.** `ros-drivers/rosserial`, branch `noetic-devel` at `c169ae2` (Jun 16, 2021, the last commit on that branch): `rosserial_client` (the MCU library), `rosserial_arduino`, `rosserial_python` (the host). Sizes were measured on an Arduino Uno build (ATmega328P) made with avr-gcc 7.3.0 and the Arduino AVR core 1.8.6 compiler flags. The build scripts and sketches are in `spike/k1-uno/`. Statements about behavior come from reading the code; none was observed on hardware.

## Summary

- rosserial costs a Uno about **9 KB of flash and 0.7 to 1.2 KB of static RAM** for one Float32 publisher and one Twist subscriber, before any stack. The K1 spike does the same job over the v3 protocol in **2.6 KB and 137 B**.
- Its RAM goes to two large frame buffers, pointer tables sized for 25 publishers and 25 subscribers, **vtables and type-name and MD5 strings that AVR keeps in RAM**, and a heap allocator linked in for array fields.
- Its receive path writes frames into a fixed buffer **without a bounds check**, and its publish path serializes **before** checking the size, so oversized or corrupted frames corrupt memory.
- Its time sync is a millisecond offset with a systematic bias and no drift model. Its reset detection relies on timeouts: up to **15 s** of outage after an MCU reset.
- The parity table in `proposal-v3.md` holds. The rosserial-compatible API needs more surface than v3 lists, and "ports unmodified apart from includes" only holds for messages without arrays or strings (see the last section).

## Architecture

- **MCU side:** `ros_lib`, a set of C++ headers generated on a ROS 1 machine (`make_libraries.py`) and copied into the Arduino libraries folder. One header per message type, with `serialize`, `deserialize`, `getType` and `getMD5` methods. The node is `ros::NodeHandle_<Hardware, MAX_SUBSCRIBERS, MAX_PUBLISHERS, INPUT_SIZE, OUTPUT_SIZE>`; the port is a `Hardware` class with `init`, `read`, `write` and `time` (milliseconds).
- **Host side:** `rosserial_python`'s `SerialClient` (a C++ `rosserial_server` also exists). It creates ROS 1 publishers and subscribers from the topic list the MCU sends.
- **Dispatch:** `Subscriber_` and `NodeHandleBase_` are abstract classes; every incoming frame goes through a virtual `callback()`.

## Wire protocol (protocol version 2, "hydro")

```
0xFF | 0xFE | len_L | len_H | len_checksum | topic_L | topic_H | payload | checksum
```

- 8 bytes of overhead per frame. Both checksums are 8-bit additive (`255 - sum % 256`).
- Payload is ROS 1 serialization: packed little-endian, `uint32` lengths for strings and arrays.
- Resync: the receiver hunts for `0xFF`; a frame not completed within 20 ms is abandoned.
- Topic IDs are 16-bit. System IDs: 0 publisher info (also the host's "send me your topics" request), 1 subscriber info, 2 and 4 service server and client info, 6 parameter request, 7 log, 10 time, 11 TX stop. Subscribers get IDs from 100 in registration order, publishers from `100 + MAX_SUBSCRIBERS`.
- Topic negotiation: when the host sends a frame on ID 0, the MCU answers with one `TopicInfo` per publisher and subscriber (ID, topic name, type name, MD5, buffer size) and a time request. The host checks the MD5 against its installed type.

## Time

- The MCU sends a time request every 2.5 s while connected and notes `millis()`. When the reply arrives, it adds the **whole** measured round trip to the host's time and stores an offset from `millis()`.
- Consequences: a systematic bias of about half the round trip (the host's stamp was taken somewhere in the middle), millisecond resolution, and no drift model. The offset is simply replaced every 2.5 s, so a Uno's resonator error (up to thousands of ppm) accumulates to several milliseconds between syncs.

## Connection loss and reset

- **MCU:** marks itself disconnected when no time reply arrives for 11 s. While disconnected, `publish()` on user topics silently returns 0.
- **Host:** declares "Lost sync with device" after three times its timeout (5 s by default, so 15 s) without a time request, then asks for the topics again.
- **Reset:** there is no session or boot identifier. A reset MCU waits silently for the host's topic request, and the host only sends one when its 15 s timeout fires. The host also opens the port with `dsrdtr=True`, which resets a Uno on every connection.

## API surface that sketches use

- `ros::NodeHandle` (or `NodeHandle_<...>` with custom sizes, which sketches do to save RAM), `initNode()`, `advertise()`, `subscribe()`, `spinOnce()`, `connected()`, `now()`, `setSpinTimeout()`, `getHardware()`.
- `ros::Publisher(name, &msg)`, `publish(&msg)`.
- `ros::Subscriber<MsgT>(name, callback)` and `ros::Subscriber<MsgT, ObjT>(name, &ObjT::method, &obj)` for member-function callbacks. Each subscriber owns a message object.
- `ros::ServiceServer<Req, Res>` and `ros::ServiceClient<Req, Res>`, `advertiseService()`, `serviceClient()`.
- `getParam(name, int*|float*|char**|bool*, length, timeout)`, which blocks and spins inside.
- `logdebug` to `logfatal` with RAM strings only (no `F()` or `PROGMEM` support anywhere in the client library).
- `ros::Time`, `ros::Duration` with arithmetic, and `tf::TransformBroadcaster`.
- Message fields: `char*` strings, variable arrays as `<field>_length` plus a pointer, `float64` as C `float` on AVR, ROS 1 `Header` with a `seq` field.

## Memory on a Uno (measured)

Static RAM is `.data + .bss`; the stack comes on top and was not measured. Flash is `.text + .data`.

| Build | Flash | Static RAM | Over baseline |
| --- | --- | --- | --- |
| Baseline: `Serial.begin`, read and write only | 1,460 B | 184 B | |
| rosserial, Uno default `NodeHandle_<..., 25, 25, 280, 280>` | 10,554 B | 1,354 B | +9,094 B flash, +1,170 B RAM |
| rosserial, smallest that still negotiates `NodeHandle_<..., 1, 1, 64, 128>` | 10,448 B | 890 B | +8,988 B flash, +706 B RAM |
| K1 spike (v3 protocol, handwritten) | 4,082 B | 321 B | +2,622 B flash, +137 B RAM |

Workload in every case: one Float32 publisher and one Twist subscriber; the spike also echoes the Twist back and answers time pings. Notes on fairness:

- rosserial always compiles in logging and the parameter response decoder, which the spike lacks; it also implements time correction, where the spike only answers pings. The gap is not all overhead, but the RAM gap in particular is structural.
- The rosserial MD5 strings were placeholders of the real length, so sizes are unaffected.
- The tuned rosserial build needs at least about 90 bytes of TX buffer, because a `TopicInfo` for `geometry_msgs/Twist` (type name and a 32-character MD5) must fit in one frame.

Where rosserial's RAM goes, beyond the two buffers and the pointer tables: one vtable per message class, per subscriber template instance and for the node handle (nine of them here, 106 bytes, all in RAM on AVR), type-name and MD5 string literals in `.data` (AVR copies them to RAM), and the `Serial` object. Flash includes `realloc` and `malloc`, pulled in by array fields, and 672 bytes of binary64 conversion code for `float64`.

## Failure modes found in the code

1. **Unbounded receive.** `message_in[index_++] = data` takes the length from the frame header (up to 65,535) and never checks it against `INPUT_SIZE`. The host checks sizes for frames it sends on purpose, but a corrupted header that passes the 8-bit length checksum (about 1 in 256) overruns the buffer.
2. **Serialize, then check.** `publish()` serializes into `message_out` and only then compares the length with `OUTPUT_SIZE`, so an oversized message has already overwritten memory when the "message larger than buffer" error is logged.
3. **Strings alias the receive buffer.** String fields are deserialized in place, pointing into `message_in`; they are valid only during the callback.
4. **Heap on a 2 KB part.** Variable-length array fields grow with `realloc` on the MCU.
5. **Weak error detection.** 8-bit additive checksums miss reordered bytes and many multi-bit errors.
6. **Blocking parameters.** `getParam()` spins until the reply or a timeout, so callbacks run inside the caller's `getParam()`.
7. **Time bias and drift**, as above.
8. **Slow reset recovery**, as above.
9. **`float64` doubled on AVR.** Every `float64` field is converted to binary64 on the wire, doubling its bytes for no precision gain, at a cost of 672 bytes of flash.
10. **Strings in RAM.** Topic names, type names, MD5 sums and log messages all occupy RAM on AVR.

## Parity table check

Every row of the parity table in `proposal-v3.md` exists in rosserial. Corrections:

- **Parameters** are get-only and blocking in rosserial. v3's plan (parameters declared by the MCU and exposed as ROS 2 parameters of the bridge, set from the host) is a superset, not parity.
- **TF** has a client-side helper (`tf::TransformBroadcaster`). Parity means providing at least that convenience, even if the work happens on the host.
- **Transports:** rosserial also runs over TCP (ESP8266 and Ethernet shields) and XBee. Out of scope for v1, as v3 says.

## Implications for v3

Already covered by v3: bounded frames with a real CRC, bounds checks on both paths, flash strings, forced narrow floats, boot-ID reset detection, drift-corrected time, the Uno DTR reset.

New, to fold into v3:

1. **The compatibility API needs `NodeHandle_<...>` with size parameters**, because sketches use it to save RAM. In ros2serial the sizes are computed, so the parameters should be accepted and ignored, with a compile-time note.
2. **Member-function subscribers** (`Subscriber<MsgT, ObjT>`), `setSpinTimeout()`, `getHardware()`, `ros::Time` and `ros::Duration` arithmetic and a `tf::TransformBroadcaster` belong in the compatibility API (ticket P11).
3. **"Unmodified apart from includes" needs scoping.** Sketches set array fields as `msg.data_length = n; msg.data = buffer;` and use `char*` strings and `header.seq`. P11 should either provide those field conventions in the compatibility headers or state that only messages with fixed-size fields port unchanged. Recommendation: fixed-size messages port unchanged; arrays, strings and `Header` get a documented one-line edit each in the migration guide.
4. **Services in the compatibility API** wait for the reliable channel (Phase 3); until then the compatibility headers should fail to compile with a clear message rather than misbehave.
