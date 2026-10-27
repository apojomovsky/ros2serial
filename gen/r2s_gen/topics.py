"""C emission: the topic table, publish wrappers, dispatch and limits.

`emit` holds the structs and `descriptors` the codec tables; this module holds
everything the topic list adds. User topic IDs run from 16 in configuration
order, each topic names its type by the descriptor index the depth-first walk
fixed, and the frame limit covers the largest user payload plus the HELLO and
TOPIC announcements the configuration sends. `generate` composes all three
into the firmware's header and source pair.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING

from r2s_gen import descriptors, emit, typehash
from r2s_gen.bounds import ResolvedMessage
from r2s_gen.msgs import MessageError, canonical_type_name

if TYPE_CHECKING:
    from r2s_gen.config import Config
    from r2s_gen.msgs import MessageRepository

FIRST_USER_TOPIC_ID = 16
LAST_USER_TOPIC_ID = 255
MAX_USER_TOPICS = LAST_USER_TOPIC_ID - FIRST_USER_TOPIC_ID + 1
FRAME_OVERHEAD = 4
FRAME_LIMIT = 65277
DEFAULT_TOPIC_MAX_COUNT = 16

_DIRECTION_FLAGS = {"pub": 0x00, "sub": 0x01}
_QOS_FLAGS = {"best_effort": 0x00, "keep_latest": 0x04, "reliable": 0x08}
_NARROW_FLAG = 0x02


@dataclass(frozen=True)
class TopicEntry:
    name: str
    type_name: str
    direction: str
    c_name: str
    topic_id: int
    type_index: int
    flags: int
    type_hash: bytes


@dataclass(frozen=True)
class TopicTable:
    node: str
    entries: tuple[TopicEntry, ...]
    max_frame: int


def c_identifier(name: str) -> str:
    """The topic name as a C identifier: ROS segments join with underscores."""
    return name.replace("/", "_")


_TAKEN = frozenset(
    {
        # The generated file's own publish helper, which a wrapper would
        # otherwise redefine.
        "one",
        # A C keyword is no identifier in the receive union, so no topic
        # takes one even when only its publish wrapper would compile. The
        # underscore keywords sit with the rest: _Bool, _Complex and
        # _Imaginary are C99, the others C11, and _Pragma is an operator.
        "auto",
        "break",
        "case",
        "char",
        "const",
        "continue",
        "default",
        "do",
        "double",
        "else",
        "enum",
        "extern",
        "float",
        "for",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "register",
        "restrict",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "struct",
        "switch",
        "typedef",
        "union",
        "unsigned",
        "void",
        "volatile",
        "while",
        "_Alignas",
        "_Alignof",
        "_Atomic",
        "_Bool",
        "_Complex",
        "_Generic",
        "_Imaginary",
        "_Noreturn",
        "_Pragma",
        "_Static_assert",
        "_Thread_local",
    }
)


def check_collisions(names: Sequence[str], source: str) -> None:
    """Reject topic names that map onto the same C identifier, naming both."""
    seen: dict[str, str] = {}
    for index, name in enumerate(names):
        ident = c_identifier(name)
        key = ident.upper()
        if key in seen:
            if seen[key] != name:
                raise MessageError(
                    f"{source}: topics[{index}].name",
                    f"topic name '{name}' collides with '{seen[key]}' as C identifier '{ident}'",
                    "rename one of the topics",
                )
        else:
            seen[key] = name
        if ident in _TAKEN:
            raise MessageError(
                f"{source}: topics[{index}].name",
                f"topic name '{name}' has no C identifier the generated file can use ('{ident}' is taken)",
                "rename the topic",
            )


def build(
    config: Config,
    resolved: Mapping[str, ResolvedMessage],
    repository: MessageRepository,
    table: descriptors.DescriptorTable | None = None,
    source: str = "r2s.yaml",
) -> TopicTable:
    """The topic table for `config`: IDs, flags, hashes and the frame limit."""
    names = [topic.name for topic in config.topics]
    check_collisions(names, source)
    if len(names) > MAX_USER_TOPICS:
        raise MessageError(
            f"{source}: topics",
            f"{len(names)} topics exceed the {MAX_USER_TOPICS} user topic IDs"
            f" ({FIRST_USER_TOPIC_ID} to {LAST_USER_TOPIC_ID})",
            "remove some topics",
        )
    roots = [canonical_type_name(topic.type_name) for topic in config.topics]
    if table is None:
        table = descriptors.build(resolved, roots)
    entries = tuple(
        TopicEntry(
            name=topic.name,
            type_name=canonical_type_name(topic.type_name),
            direction=topic.direction,
            c_name=c_identifier(topic.name),
            topic_id=FIRST_USER_TOPIC_ID + position,
            type_index=table.index_of[canonical_type_name(topic.type_name)],
            flags=_flags(topic.direction, topic.qos, topic.narrow_float64),
            type_hash=_hash(repository, topic.type_name),
        )
        for position, topic in enumerate(config.topics)
    )
    return TopicTable(node=config.node, entries=entries, max_frame=_max_frame(config, resolved, entries, source))


def _flags(direction: str, qos: str, narrow: bool) -> int:
    """The announced CTL-4 flags without the bit the build forces (ENC-12)."""
    flags = _DIRECTION_FLAGS[direction] | _QOS_FLAGS[qos]
    if narrow:
        flags |= _NARROW_FLAG
    return flags


def _hash(repository: MessageRepository, type_name: str) -> bytes:
    """The first 8 bytes of the type's RIHS01 hash, as CTL-12 carries them."""
    digest = typehash.type_hash(repository, type_name)
    return bytes.fromhex(digest[len("RIHS01_") :])[:8]


def _varint_len(value: int) -> int:
    if value < 128:
        return 1
    if value < 16384:
        return 2
    return 3


def _payload(resolved: Mapping[str, ResolvedMessage], name: str, narrow: bool, memo: dict) -> int:
    """The largest wire payload of `name`: bounds, not values, set the size."""
    key = (name, narrow)
    if key not in memo:
        memo[key] = sum(_field_wire(resolved, field, narrow, memo) for field in resolved[name].fields)
    return memo[key]


def _field_wire(resolved: Mapping[str, ResolvedMessage], field, narrow: bool, memo: dict) -> int:
    if field.type_name == "string":
        element = _varint_len(field.string_bound) + field.string_bound
    elif field.type_name in descriptors._ELEMENT_BYTES:
        element = descriptors._ELEMENT_BYTES[field.type_name]
        if field.type_name == "float64" and narrow:
            element = 4
    else:
        element = _payload(resolved, field.type_name, narrow, memo)
    if not field.is_array:
        return element
    if not field.is_sequence:
        return field.array_bound * element
    return _varint_len(field.array_bound) + field.array_bound * element


def _string_wire(text: str) -> int:
    return _varint_len(len(text)) + len(text)


def _max_frame(config: Config, resolved: Mapping[str, ResolvedMessage], entries: tuple, source: str) -> int:
    """The largest decoded frame the firmware sends or receives, overhead included."""
    memo: dict = {}
    configured = {entry.name: (entry.type_name, entry.flags & _NARROW_FLAG != 0) for entry in entries}
    user = 0
    for name in configured:
        type_name, narrow = configured[name]
        user = max(user, _payload(resolved, type_name, narrow, memo))
    hello = 17 + _string_wire(config.node)
    announced = max([hello] + [12 + _string_wire(entry.type_name) + _string_wire(entry.name) for entry in entries])
    need = max(user, announced) + FRAME_OVERHEAD
    if need > FRAME_LIMIT:
        raise MessageError(
            f"{source}: topics",
            f"the configuration needs a max_frame of {need}, past the protocol limit of {FRAME_LIMIT}",
            "shrink a bound or split the topics",
        )
    override = config.max_frame
    if override is None:
        return need
    if override > FRAME_LIMIT:
        raise MessageError(
            f"{source}: limits.max_frame",
            f"max_frame {override} exceeds the protocol limit of {FRAME_LIMIT}",
            "lower it",
        )
    if override < need:
        raise MessageError(
            f"{source}: limits.max_frame",
            f"max_frame {override} is below the {need} the configuration needs",
            "raise it",
        )
    return override


def _call_lines(first: str, args: list, last: str, limit: int = 110) -> list:
    """A call wrapped the way clang-format wraps one: fill every line, continue at the paren."""
    cont = " " * len(first)
    lines: list = []
    line = first
    for position, arg in enumerate(args):
        comma = "," if position < len(args) - 1 else ""
        gap = "" if line in (first, cont) else " "
        piece = f"{gap}{arg}{comma}"
        rest = "" if comma else last
        if len(line) + len(piece) + len(rest) <= limit:
            line += piece
            if not comma:
                line += rest
        else:
            lines.append(line)
            line = f"{cont}{arg}{comma}"
            if not comma:
                line += rest
    lines.append(line)
    return lines


def emit_h(table: TopicTable) -> str:
    """The topic section of the generated header: limits, IDs, the table and the wrappers."""
    lines = [
        "/* Topics in configuration order: user IDs run from 16 (FRM-3), each",
        " * topic names its type by the descriptor index the depth-first walk",
        " * fixed (DSC-11), and the flags carry direction, narrow, keep-latest",
        " * and reliable (CTL-4) without the narrow bit the build forces. */",
        "",
        f"#define R2S_TOPIC_COUNT {len(table.entries)}u",
        "#ifndef R2S_MAX_FRAME",
        f"#define R2S_MAX_FRAME {table.max_frame}u",
        "#endif",
    ]
    if len(table.entries) > DEFAULT_TOPIC_MAX_COUNT:
        lines.extend(
            [
                "#ifndef R2S_TOPIC_MAX_COUNT",
                f"#define R2S_TOPIC_MAX_COUNT {len(table.entries)}u",
                "#endif",
            ]
        )
    lines.extend(
        [
            "",
            '#include "r2s/r2s_codec.h"',
            '#include "r2s/r2s_session.h"',
            '#include "r2s/r2s_topics.h"',
            "",
        ]
    )
    for entry in table.entries:
        lines.append(f"#define R2S_TOPIC_{entry.c_name.upper()}_ID {entry.topic_id}u")
    for entry in table.entries:
        lines.append(f"#define R2S_TOPIC_{entry.c_name.upper()}_TYPE {entry.type_index}u")
    for entry in table.entries:
        if entry.direction == "pub":
            lines.extend(
                [
                    "",
                    f"/* Publishes on {entry.name}. Fails with the not-connected error while",
                    " * announcing, with the rejected error when the host did not accept",
                    " * the topic, and with the bounds error when the message does not",
                    " * fit, sending nothing in each case. */",
                    f"r2s_status_t r2s_publish_{entry.c_name}(const {emit._c_name(entry.type_name)} *msg);",
                ]
            )
    for entry in table.entries:
        if entry.direction == "sub":
            lines.extend(
                [
                    "",
                    f"/* Handles {entry.name}; the application implements it. Dispatch calls",
                    " * it from r2s_spin_some(), never from inside a publish. */",
                    f"void r2s_on_{entry.c_name}(const {emit._c_name(entry.type_name)} *msg);",
                ]
            )
    lines.extend(
        [
            "",
            "/* The announced topics and the descriptor table the wrappers encode",
            " * with; a board that renames topics at launch copies this table. */",
            "extern const r2s_topic_t R2S_FLASH r2s_topics[R2S_TOPIC_COUNT];",
            "extern const r2s_codec_table_t r2s_generated_table;",
        ]
    )
    return "\n".join(lines)


def emit_c(table: TopicTable) -> str:
    """The topic section of the generated source: the tables, the wrappers and dispatch."""
    lines = [
        "",
        "#include <float.h>",
        "",
        '#include "r2s/r2s_framer.h"',
        '#include "r2s/r2s_session.h"',
        '#include "r2s/r2s_tx.h"',
        "",
        "/* A build whose double is 32 bits travels narrow on every topic (ENC-12). */",
        "#if DBL_MANT_DIG == FLT_MANT_DIG",
        "#define R2S_NARROW_FLAG 0x02u",
        "#else",
        "#define R2S_NARROW_FLAG 0x00u",
        "#endif",
        "",
        "/* The announced topics (CTL-4, CTL-12): the hashes are the first 8 bytes",
        " * of each type's RIHS01 hash, and R2S_NARROW_FLAG carries ENC-12. */",
        "const r2s_topic_t R2S_FLASH r2s_topics[R2S_TOPIC_COUNT] = {",
    ]
    for entry in table.entries:
        digest = ", ".join(f"0x{byte:02x}u" for byte in entry.type_hash)
        lines.extend(
            [
                f"    {{R2S_TOPIC_{entry.c_name.upper()}_ID,",
                f"     (uint8_t)(0x{entry.flags:02x}u | R2S_NARROW_FLAG),",
                f"     {{{digest}}},",
                f'     "{entry.type_name}",',
                f'     "{entry.name}"}},',
            ]
        )
    lines.extend(
        [
            "};",
            "",
            "const r2s_codec_table_t r2s_generated_table = {",
            "    r2s_type_records,",
            "    R2S_TYPE_COUNT,",
            "    r2s_field_entries,",
            "    R2S_FIELD_COUNT,",
            "};",
        ]
    )
    if any(entry.direction == "pub" for entry in table.entries):
        lines.extend(
            [
                "",
                "/* One publish path for every wrapper: measures the payload, reserves",
                " * its wire form, then streams it, so a failed publish writes nothing. */",
                "static r2s_status_t r2s_publish_one(uint8_t topic_id, uint8_t index, uint16_t type, const void *msg)",
                "{",
                "    r2s_codec_out_t out = {NULL, 0u, 0u, 0u};",
                "    uint8_t narrow;",
                "    uint8_t field = 0xFFu;",
                "    uint8_t seq;",
                "    int8_t rc;",
                "",
                "    if (r2s_connected() == 0u) {",
                "        return R2S_ERR_NOT_CONNECTED;",
                "    }",
                "    if (index == 0xFFu || r2s_session_topic_status(index) != 0u) {",
                "        return R2S_ERR_TOPIC_REJECTED;",
                "    }",
                "    narrow = (uint8_t)(r2s_session_topic_flags(index) & 0x02u);",
                "    rc = r2s_codec_encode(&r2s_generated_table, type, msg, narrow, &out, &field);",
                "    if (rc != R2S_OK) {",
                "        return rc;",
                "    }",
                "    seq = r2s_session_tx_seq(index);",
                "    rc = r2s_tx_begin(topic_id, seq, out.len);",
                "    if (rc != R2S_OK) {",
                "        return rc;",
                "    }",
                "    r2s_session_advance_tx_seq(index);",
                "    out.to_tx = 1u;",
                "    out.len = 0u;",
                "    (void)r2s_codec_encode(&r2s_generated_table, type, msg, narrow, &out, &field);",
                "    (void)r2s_tx_end();",
                "    return R2S_OK;",
                "}",
            ]
        )
    for entry in table.entries:
        if entry.direction == "pub":
            lines.extend(
                [
                    "",
                    f"r2s_status_t r2s_publish_{entry.c_name}(const {emit._c_name(entry.type_name)} *msg)",
                    "{",
                    *_call_lines(
                        "    return r2s_publish_one(",
                        [
                            f"R2S_TOPIC_{entry.c_name.upper()}_ID",
                            f"r2s_session_find_topic(R2S_TOPIC_{entry.c_name.upper()}_ID)",
                            f"R2S_TOPIC_{entry.c_name.upper()}_TYPE",
                            "msg",
                        ],
                        ");",
                    ),
                    "}",
                ]
            )
    subs = [entry for entry in table.entries if entry.direction == "sub"]
    if subs:
        lines.extend(
            [
                "",
                "/* One receive buffer shared by every subscription: only one dispatch",
                " * runs at a time, so one slot is enough. */",
                "typedef union r2s_rx_buf {",
            ]
        )
        for entry in subs:
            lines.append(f"    {emit._c_name(entry.type_name)} {entry.c_name};")
        lines.extend(
            [
                "} r2s_rx_buf_t;",
                "",
                "static r2s_rx_buf_t r2s_rx_buf;",
                "",
                "/* Decodes the subscribed topics into the buffer above and calls the",
                " * handler of the frame's topic; anything else is refused and counted. */",
                "void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)",
                "{",
                "    uint8_t index;",
                "    uint8_t narrow;",
                "    uint8_t field = 0xFFu;",
                "",
                "    switch (topic_id) {",
            ]
        )
        for entry in subs:
            macro = entry.c_name.upper()
            lines.extend(
                [
                    f"    case R2S_TOPIC_{macro}_ID: {{",
                    f"        index = r2s_session_find_topic(R2S_TOPIC_{macro}_ID);",
                    "        if (index == 0xFFu || r2s_session_topic_status(index) != 0u) {",
                    "            r2s_framer_count_refused();",
                    "            return;",
                    "        }",
                    "        narrow = (uint8_t)(r2s_session_topic_flags(index) & 0x02u);",
                    "        (void)r2s_session_note_rx_seq(index, seq);",
                    *_call_lines(
                        "        if (r2s_codec_decode(",
                        [
                            "&r2s_generated_table",
                            f"R2S_TOPIC_{macro}_TYPE",
                            "payload",
                            "len",
                            "narrow",
                            f"&r2s_rx_buf.{entry.c_name}",
                            "&field",
                        ],
                        ") != R2S_OK) {",
                    ),
                    "            r2s_framer_count_refused();",
                    "            return;",
                    "        }",
                    f"        r2s_on_{entry.c_name}(&r2s_rx_buf.{entry.c_name});",
                    "        return;",
                    "    }",
                ]
            )
        lines.extend(
            [
                "    default: {",
                "        r2s_framer_count_refused();",
                "        return;",
                "    }",
                "    }",
                "}",
            ]
        )
    else:
        lines.extend(
            [
                "",
                "/* No subscriptions: every user frame is refused and counted. */",
                "void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)",
                "{",
                "    (void)topic_id;",
                "    (void)seq;",
                "    (void)payload;",
                "    (void)len;",
                "    r2s_framer_count_refused();",
                "}",
            ]
        )
    lines.append("")
    return "\n".join(lines)


def generate(
    config: Config, resolved: Mapping[str, ResolvedMessage], repository: MessageRepository, source: str = "r2s.yaml"
) -> tuple[str, str]:
    """The firmware's header and source pair for `config`, descriptors first."""
    roots = [canonical_type_name(topic.type_name) for topic in config.topics]
    table = descriptors.build(resolved, roots)
    topic_table = build(config, resolved, repository, table, source)
    base = emit.emit_types(resolved, max_nest=table.max_nest)
    closing = "#endif\n"
    assert base.endswith(closing)
    header = base[: -len(closing)] + emit_h(topic_table) + "\n\n" + closing
    return header, descriptors.emit_c(table) + emit_c(topic_table)
