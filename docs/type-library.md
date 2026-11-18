# The library route and its config boundary

`r2s-gen types` writes a library for your own interface types, so a sketch
declares a topic, service or action with the same rows a bundled type uses. This
page says what the library carries, which settings it cannot carry, and when the
copy in your firmware project goes stale.

Read [custom interfaces](custom-interfaces.md) first if you have not generated
anything yet; this page starts where that one ends.

## What the library carries

A library is the same shape as the bundled one, with your types in place of the
bundled list:

- `ros2serial.h`, the sketch header that expands `R2S_TOPICS` and `R2S_ACTIONS`.
- `r2s_bundled.h` and `r2s_bundled.c`, the descriptors, hashes and frame limit
  for the types it carries.
- One message header per type, at the `<pkg>/msg/<type>.hpp` path, plus a
  service or action header for each service or action named.
- `library.properties`, whose `name` carries a digest of the type list and its
  bounds, so two different lists never share a name.

There is no `r2s.yaml` and no topic list. The input is the types and their
bounds:

```sh
ros2 run r2s_gen r2s-gen types my_robot_interfaces/msg/SensorReading \
  --msg-path ~/ros2_ws/src --out firmware/lib
```

A sketch then names the type like a bundled one. Nothing in the row says where
the type came from:

```text
#define R2S_NODE_NAME "sensor_board"
#define R2S_TOPICS(X) \
    X(Publisher, my_robot_interfaces::msg::SensorReading, reading_pub, "reading")
#include <ros2serial.h>
```

## What the library cannot carry

The library is a description of types. It is not a description of a node, so
four settings that `r2s.yaml` holds have no home here. When a firmware needs one
of them, it keeps an `r2s.yaml` and uses the generated static API instead.

| Setting | Why the library cannot carry it | Where it lives |
| --- | --- | --- |
| `limits.max_frame` | The limit sizes the core's own buffers, which compile in the library and in your sketch, so every translation unit must agree on it | A build flag, or the limit the library's header declares |
| `bounds` for a field you cannot edit | A bound is a decision about storage, and the library takes it per type on the command line or in the type list | `--bound`, or the type list's `bounds` |
| `features` and `tier` | `r2s.yaml` records both in the generated header's config hash; no macro reaches the C core from them yet, since the feature macros are still build flags | The configuration hash, with `-DR2S_WITH_*` flags on every unit for the channel itself |
| Service and action indices | The static C hook API switches on generated `R2S_SERVICE_*_ID` and `R2S_ACTION_*_ID` values, which only `r2s-gen generate` emits | The generated static API |

Two more limits of the route, stated so a switch is not a surprise:

- A type library carries headers, not entity tables: the service header names the
  request and response pair and its hash, and the action header carries the
  action struct and its `traits`, but the tables that `r2s_services_configure()`
  binds come from `r2s-gen generate`.
- `--layout` picks the wrapper (`library.properties`, `library.json` or a
  `CMakeLists.txt`), not a language: the emitted C is the same in all three.

## The frame limit, which both sides must agree on

`R2S_MAX_FRAME` sizes the receive buffer inside the core's own source files.
Those files never see a `#define` in your sketch, so the value has to reach
every translation unit of the core, the port and your sketch at once. A build
that leaves it out fails with:

```text
R2S_MAX_FRAME is not set: pass -DR2S_MAX_FRAME=<n> to every translation unit
```

The library's `r2s_bundled.h` declares a limit for the types it carries. Pass it
to the build the way [the firmware guide](firmware.md#the-frame-limit-is-a-build-input)
shows for your toolchain. A limit larger than the one the header declares is
your call; a limit smaller than a row's frame fails the build at that row,
naming the object.

The [firmware guide](firmware.md#the-frame-limit-is-a-build-input) owns the
per-toolchain table; this page only says why the value is a build input rather
than a line in the sketch.

## Bounds

A variable-length field takes its bound from exactly one place: its `.msg`
definition (`string<=12`, `T[<=N]`), or the list beside the type, which is
`--bound` on the command line or `bounds` in the type list file. With neither,
generation fails and names the field.

An override on a field the `.msg` already bounds is an error in both directions,
because a tighter buffer under-holds a message the host may legally send and a
looser one accepts a message ROS calls malformed. [Message bounds](message-bounds.md)
states the rule, the conflict case and the built-in types' limits; this page
does not repeat them.

## Staleness: regenerate after a `.msg` change

The type hashes travel inside the library. They are what the firmware announces
in its handshake and what the bridge compares against the host's own type
support, so a library built from an older `.msg` compiles cleanly and then makes
the bridge reject every topic of that type at handshake.

The rule: after any change to a `.msg`, `.srv` or `.action` file that the
library carries, run `r2s-gen types` again with the same arguments and rebuild.

Two helps:

- The library's name and `library.properties` carry the digest of its type list
  and its bounds, so a rebuilt library never overwrites an older one silently
  under the same name.
- `--write-config r2s-types.yaml` writes the equivalent type list from a command
  you already ran, so the list is reviewable in the repository and the rebuild
  is one command.

```sh
ros2 run r2s_gen r2s-gen types --types-file r2s-types.yaml \
  --msg-path ~/ros2_ws/src --out firmware/lib
```

## The distro

Type hashes differ between ROS distros, so the distro that resolves the types
must be the distro the bridge runs on. The generator resolves it in this order:

1. `--distro <name>` on the command line.
2. `ROS_DISTRO`, which a sourced ROS environment sets.
3. The default, `jazzy`.

For a type list file, the file's own `distro:` key is read before the
environment. When ROS is sourced and its distro differs from the one in use, the
generator warns, because that mismatch is exactly how a stale-hash handshake
happens.

## Where to go next

- Choose between the routes: [choosing how to declare your topics](declaring-topics.md).
- Generate with a topic list instead: [custom interfaces](custom-interfaces.md).
- Set a field's storage: [message bounds](message-bounds.md).
- Read the schema: [the design](00-design.md#code-generator-specification).
