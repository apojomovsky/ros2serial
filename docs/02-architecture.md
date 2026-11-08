# ros2serial architecture

A map from the repository to the running system, for contributors. The
specification stays in [00-design.md](00-design.md) and
[spec/protocol.md](../spec/protocol.md); this page shows where each part
of it lives in the tree. Every name below is a real directory, file or
symbol on `master`.

## Context

```mermaid
flowchart LR
    sketch["Sketch or app<br/>(examples/arduino_minimal)"]
    gen["r2s-gen, build time<br/>(gen/r2s_gen)"]
    mcu["MCU firmware<br/>(core/ + ports/ + cpp/)"]
    link["Serial link<br/>(rCOBS frames + CRC)"]
    bridge["Bridge node<br/>(bridge/r2s_bridge)"]
    ros["ROS 2 graph<br/>(any RMW)"]

    sketch --> mcu
    gen -. "r2s_generated.h/.c" .-> mcu
    mcu <--> link
    link <--> bridge
    bridge <--> ros
```

A sketch publishes and subscribes through the core or the C++ layer,
and the firmware exchanges rCOBS frames with one bridge node over a
serial link. The bridge republishes the MCU topics on the ROS 2 graph
and forwards subscriptions back. `r2s-gen` runs at build time only: it
turns the topic list into C sources the firmware compiles, so no Python
ships on either side of the wire. The wire today is protocol v2
([GEN-2] in `spec/protocol.md`). Parameters (ID 3) and the
reliable-channel ack (ID 6) stay reserved under [SYS-3] pending S5, and
services and actions stay out pending S8, so no section here claims
their behavior. See [Part I, Architecture](00-design.md#architecture)
and [Part I, Declaring topics](00-design.md#declaring-topics).

## Containers

```mermaid
flowchart TB
    subgraph mcu["MCU side"]
        core["Portable core (core/)<br/>framing, codec, session"]
        port["Port glue (ports/)<br/>serial + tick"]
        cxx["C++ layer (cpp/)<br/>r2s::Node and friends"]
        genout["Generated sources<br/>(r2s_generated.h/.c)"]
    end
    subgraph host["Host side"]
        br["Bridge (bridge/r2s_bridge)<br/>session, convert, route"]
        ros2["ROS 2 graph"]
    end
    spec["Spec + vectors (spec/)"]
    tool["r2s-gen (gen/r2s_gen)"]

    tool --> genout
    genout --> core
    port --> core
    cxx --> core
    core <--> br
    br <--> ros2
    spec -. "vectors check" .-> core
    spec -. "vectors check" .-> br
    spec -. "reference codec" .-> tool
```

The core compiles with no ROS and no hardware: ports implement the
`r2s_port.h` contract (`r2s_port_read`, `r2s_port_write`,
`r2s_port_tx_free`, `r2s_port_tick`), with `_link` variants where a port
hosts several links, and the C++ layer calls the same C
API a C sketch calls. The bridge reuses the core framing sources
(`core/src/framer.c`, `rcobs.c`, `crc.c`) compiled for the host, so
framing has one implementation, with one framer instance per live link
(up to 16 per bridge process). `spec/vectors/` checks all
three implementations. Launch arguments place the node in a namespace
and remap the MCU topics. See [Part II, Repository layout](00-design.md#repository-layout)
and [Part II, Test strategy](00-design.md#test-strategy).

| Box | Directory | Test scope |
| --- | --- | --- |
| Portable core | `core/` | `CORE=1` |
| Port glue | `ports/arduino`, `ports/posix` | `E2E=posix`, `E2E=avr` |
| C++ layer | `cpp/` | `CPP=1` |
| Generated sources | `gen/` output, `r2s_generated.h` | `GEN=1` |
| Bridge | `bridge/` | `BRIDGE=1` |
| Spec and vectors | `spec/` | every scope, via vectors |
| Generator | `gen/r2s_gen` | `GEN=1` |

`ports/avr`, `ports/stm32`, `ports/ch32v` and `ports/epic-hal` hold the
planned 32-bit and PIC ports; each currently carries only a `.gitkeep`.

## Generation pipeline

```mermaid
flowchart LR
    cfg["r2s.yaml<br/>(schema: r2s.schema.json)"]
    chk["r2s-gen check<br/>(config.py)"]
    gen["r2s-gen generate<br/>(layouts.py)"]
    out["r2s_generated.h/.c<br/>(+ CMakeLists or library.properties)"]
    xmacro["X-macro list<br/>(R2S_TOPICS)"]
    fw["Firmware build<br/>(arduino or cmake layout)"]
    wire["TOPIC frames<br/>(type_name + type_hash)"]
    brv["Bridge view<br/>(Session + TOPIC_STATUS)"]

    cfg --> chk
    cfg --> gen
    gen --> out
    out --> fw
    xmacro --> fw
    fw --> wire
    wire --> brv
```

`r2s-gen check` validates `r2s.yaml` without emitting anything, and
`r2s-gen generate --layout arduino|cmake` emits the structs (`emit.py`),
the descriptors (`descriptors.py`), the topic table and publish
wrappers (`topics.py`), with bounds resolved by `bounds.py`. Layouts today
are `arduino` and `cmake`; `compact` handshakes need the node map, which
`generate` does not emit yet, so use `handshake: full`. The
firmware announces each topic in a TOPIC frame; the bridge resolves the
type support, compares the hash, and answers TOPIC_STATUS. Sketches that
skip the generator use the X-macro list instead and announce the same
frames. C++ output (`--cpp`) is not emitted yet. See
[Part I, Declaring topics](00-design.md#declaring-topics) and
[Part II, Code generator specification](00-design.md#code-generator-specification).

## Following one message

```mermaid
sequenceDiagram
    participant S as Sketch
    participant C as Core (codec.c, tx.c)
    participant L as Serial bytes
    participant B as Bridge (FrameReader, Converter)
    participant R as ROS 2 topic

    S->>C: r2s_publish_battery(msg)
    C->>C: r2s_codec_encode + CRC + rCOBS
    C->>L: r2s_port_write chunks
    L->>B: frame bytes
    B->>B: FrameReader, CRC check
    B->>B: Converter packed to CDR
    B->>R: generic publish
    R->>B: generic subscription
    B->>B: Converter CDR to packed, Scheduler
    B->>L: transport write
    L->>C: frame bytes
    C->>C: framer poll, decode, dispatch
    C->>S: r2s_on_cmd_vel(msg)
```

A publish serializes field by field from the descriptors, appends the
CRC, and encodes rCOBS straight to the port in `R2S_TX_CHUNK` pieces; a
full link returns `R2S_ERR_TX_BUSY` instead of blocking. The bridge
decodes with the same core sources, converts packed to CDR with cached
plans, and publishes through a generic publisher. The subscribe path
mirrors it: CDR to packed, the Scheduler rate-limits and round-robins
host to MCU traffic, and `r2s_spin_some` dispatches to the
`r2s_on_<name>` handler. System channels use reserved topic IDs
(heartbeat is ID 4); user topics start at 16. See
[Part I, Wire encoding](00-design.md#wire-encoding-packed-converted-on-the-host),
[Part I, Receiving and sending bytes](00-design.md#receiving-and-sending-bytes),
and [Part II, Implementation pointers](00-design.md#implementation-pointers).

## Session state machine

The diagrams below follow `spec/protocol.md`, sections 8.1 and 8.2. In
code the MCU states are the `ANNOUNCING` and `CONNECTED` values in
`core/src/session.c` (`BOOT` is the init path that sends the first
HELLO), and the host states are the `SessionState` values in
`bridge/r2s_bridge/include/r2s_bridge/session.hpp`. The reliable
channel rides these two machines without claiming a transition here.
Pending work adds rules the same way: parameters in #155, services in
#156, actions in #317 to #320.

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> ANNOUNCING : init, send HELLO
    ANNOUNCING --> CONNECTED : READY
    ANNOUNCING --> ANNOUNCING : 1 s silent, send HELLO
    CONNECTED --> ANNOUNCING : link timeout or HELLO_REQ
```

```mermaid
stateDiagram-v2
    [*] --> DISCONNECTED
    DISCONNECTED --> BOOT_WAIT : port open
    BOOT_WAIT --> HANDSHAKING : HELLO or boot_wait_ms
    HANDSHAKING --> CONNECTED : topics resolved, READY sent
    CONNECTED --> HANDSHAKING : link timeout or reset
    HANDSHAKING --> DISCONNECTED : port error
    CONNECTED --> DISCONNECTED : port error
```

While announcing, the MCU answers any non-handshake host frame with a
rate-limited HELLO and its publish calls fail with
`R2S_ERR_NOT_CONNECTED`. While connected it heartbeats every
`R2S_HEARTBEAT_MS` and publishes for topics the host accepted. The host
waits `boot_wait_ms` after opening the port (a Uno resets on open),
walks one TOPIC_REQ per index with retries, sends READY with a nonzero
token, then heartbeats and runs the time sync (PING, PONG,
CORRECTION on ID 1). A HELLO with a new token means the MCU reset. See
[Part I, Protocol draft](00-design.md#protocol-draft-v1) and
[Part II, Protocol details](00-design.md#protocol-details).
