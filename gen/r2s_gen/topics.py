"""C emission: the topic table, publish wrappers, dispatch and limits.

`emit` holds the structs and `descriptors` the codec tables; this module holds
everything the topic list adds. User topic IDs run from 16 in configuration
order, each topic names its type by the descriptor index the depth-first walk
fixed, and the frame limit covers the largest user payload plus the HELLO and
TOPIC announcements the configuration sends. `generate` composes all three
into the firmware's header and source pair.
"""

from __future__ import annotations

import hashlib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING

from r2s_gen import descriptors, emit, typehash
from r2s_gen.bounds import ResolvedMessage
from r2s_gen.msgs import MessageError, canonical_type_name
from r2s_gen.version import __version__

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


def check_non_empty(names: Sequence[str], source: str) -> None:
    """Reject an empty topic list: a node with no topics has nothing to announce."""
    if not names:
        raise MessageError(
            f"{source}: topics",
            "a node with no topics has nothing to announce",
            "add at least one topic",
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
    check_non_empty(names, source)
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


def _indent_of(line: str) -> str:
    """The leading spaces of a line."""
    return line[: len(line) - len(line.lstrip(" "))]


def _block_cont(first: str) -> str:
    """The continuation indent after an open paren: one level per open scope."""
    depth = max(1, first.count("(") - first.count(")"))
    return _indent_of(first) + "    " * depth


def _arg_text(arg) -> str:
    """The source of an argument: plain text, or head(inner)tail for a call."""
    if isinstance(arg, str):
        return arg
    head, sub, tail = arg
    return f"{head}({', '.join(sub)}){tail}"


def _flush(lines: list, line: str) -> None:
    """Append a built line, unless it holds nothing but its indent."""
    if line.strip():
        lines.append(line)


def _split_member(arg: str) -> tuple | None:
    """An address-of-member argument as (receiver, member), else None."""
    head, dot, member = arg.partition(".")
    if not dot or not head.startswith("&") or not member:
        return None
    if "(" in arg or ")" in arg:
        return None
    return head, member


def _fill(start: str, cont: str, args: list, last: str, limit: int) -> list:
    """Greedy bin-pack of args; a call or member access splits when it fits nowhere whole."""
    lines: list = []
    line = start
    fresh = True
    for position, arg in enumerate(args):
        comma = "," if position < len(args) - 1 else ""
        rest = last if position == len(args) - 1 else ""
        text = _arg_text(arg) + comma
        gap = "" if fresh else " "
        if len(line) + len(gap) + len(text) + len(rest) <= limit:
            line += gap + text + rest
            fresh = False
            continue
        if len(cont) + len(text) + len(rest) <= limit:
            _flush(lines, line)
            line = cont + text + rest
            fresh = False
            continue
        member = _split_member(arg) if isinstance(arg, str) else None
        if member is not None:
            _flush(lines, line)
            lines.append(cont + member[0])
            line = cont + "     ." + member[1] + comma + rest
            fresh = False
            continue
        if not isinstance(arg, str):
            head, sub, tail = arg
            opener = head + "("
            if not fresh and len(line) + 1 + len(opener) <= limit:
                head_start = line + " " + opener
            else:
                _flush(lines, line)
                head_start = cont + opener
            inner = _call_lines(head_start, sub, tail + comma + rest, limit)
            lines.extend(inner[:-1])
            line = inner[-1]
            fresh = False
            continue
        _flush(lines, line)
        line = cont + text + rest
        fresh = False
    lines.append(line)
    return lines


def _call_lines(first: str, args: list, last: str, limit: int = 110) -> list:
    """A call wrapped the way clang-format wraps one.

    A single line when it fits, else the fewest lines of the two shapes
    clang-format chooses between: arguments kept on the first line and
    continued at the paren, or broken after the paren at one level per
    open scope. Ties keep the arguments on the first line, which matches
    the short names already in the tree.
    """
    single = first + ", ".join(_arg_text(arg) for arg in args) + last
    if len(single) <= limit:
        return [single]
    aligned = _fill(first, " " * len(first), args, last, limit)
    cont = _block_cont(first)
    block = [first] + _fill(cont, cont, args, last, limit)
    fitting = [shape for shape in (aligned, block) if all(len(line) <= limit for line in shape)]
    if fitting:
        return min(fitting, key=len)
    return block


def _decl_lines(ret: str, name: str, params: str, last: str, limit: int = 110) -> list:
    """A declaration wrapped the way clang-format wraps one.

    A single line when it fits, else after the return type when the rest
    fits and the type is longer than five characters, else after the open
    paren. Short types stay on the first line; the split sits between the
    five and six character types probed.
    """
    if len(f"{ret} {name}({params}){last}") <= limit:
        return [f"{ret} {name}({params}){last}"]
    if len(ret) > 5 and len(f"{name}({params}){last}") <= limit:
        return [ret, f"{name}({params}){last}"]
    head = f"{ret} {name}("
    if len(ret) <= 5 or len(head) <= limit:
        return [head, f"    {params}{last}"]
    return [ret, f"{name}(", f"    {params}{last}"]


def _assign_lines(indent: str, lhs: str, head: str, arg: str, last: str, limit: int = 110) -> list:
    """An assignment wrapped the way clang-format wraps one: unbroken, then
    after the equals sign, then after the call paren."""
    if len(f"{indent}{lhs} = {head}{arg}{last}") <= limit:
        return [f"{indent}{lhs} = {head}{arg}{last}"]
    if len(f"{indent}    {head}{arg}{last}") <= limit:
        return [f"{indent}{lhs} =", f"{indent}    {head}{arg}{last}"]
    return _call_lines(f"{indent}{lhs} = {head}", [arg], last, limit)


def _member_lines(indent: str, kind: str, name: str, last: str = ";", limit: int = 110) -> list:
    """A struct member wrapped the way clang-format wraps one: after the type."""
    if len(f"{indent}{kind} {name}{last}") <= limit:
        return [f"{indent}{kind} {name}{last}"]
    return [f"{indent}{kind}", f"{indent}    {name}{last}"]


def _string_chunks(content: str, cap: int) -> list:
    """Split content into clang-format string pieces: longest prefix ending after a non-alphanumeric."""
    piece = content[:cap]
    cut = len(piece)
    for at in range(len(piece), 0, -1):
        if not piece[at - 1].isalnum():
            cut = at
            break
    if cut >= len(content):
        cut = min(len(content) - 1, cap)
    return [content[:cut], content[cut:]]


def _string_row(indent: str, content: str, last: str, limit: int = 110) -> list:
    """A table string row wrapped the way clang-format wraps one: split the literal."""
    if len(f'{indent}"{content}"{last}') <= limit:
        return [f'{indent}"{content}"{last}']
    cap = limit - len(indent) - 2
    final = cap - len(last)
    chunks: list = []
    rest = content
    while len(rest) > final:
        head, rest = _string_chunks(rest, cap)
        chunks.append(head)
    chunks.append(rest)
    rows = [f'{indent}"{chunks[0]}"']
    rows.extend(f'{indent}"{chunk}"' for chunk in chunks[1:-1])
    rows.append(f'{indent}"{chunks[-1]}"{last}')
    return rows


def _define_lines(name: str, value: str, limit: int = 110) -> list:
    """A define wrapped the way clang-format wraps one: backslash in column 110."""
    if len(f"#define {name} {value}") <= limit:
        return [f"#define {name} {value}"]
    gap = max(1, limit - len(f"#define {name}") - 1)
    return [f"#define {name}{' ' * gap}\\", f"    {value}"]


def _comment_lines(first: str, cont: str, last: str, text: str, limit: int = 110) -> list:
    """Greedy word fill of a comment paragraph, first word always placed."""
    words = text.split(" ")
    rows: list = []
    line = first
    fresh = True
    for position, word in enumerate(words):
        ending = last if position == len(words) - 1 else ""
        if fresh:
            line += word
            fresh = False
        elif len(line) + 1 + len(word) + len(ending) <= limit:
            line += " " + word
        else:
            rows.append(line)
            line = cont + word
    rows.append(line + last)
    return rows


def _doc_lines(template: list, text: str, limit: int = 110) -> list:
    """A doc comment wrapped the way clang-format wraps one.

    Kept when it fits, else refilled greedily. The refill is clang-format's
    own reflow shape, so it is stable wherever the comment lands.
    """
    if all(len(row) <= limit for row in template):
        return template
    return _comment_lines("/* ", " * ", " */", text, limit)


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
            '#include "r2s/r2s_port.h"',
            '#include "r2s/r2s_session.h"',
            '#include "r2s/r2s_topics.h"',
            "",
        ]
    )
    for entry in table.entries:
        lines.extend(_define_lines(f"R2S_TOPIC_{entry.c_name.upper()}_ID", f"{entry.topic_id}u"))
    for entry in table.entries:
        lines.extend(_define_lines(f"R2S_TOPIC_{entry.c_name.upper()}_TYPE", f"{entry.type_index}u"))
    for entry in table.entries:
        if entry.direction == "pub":
            lines.extend(
                [
                    "",
                    *_doc_lines(
                        [
                            f"/* Publishes on {entry.name}. Fails with the not-connected error while",
                            " * announcing, with the rejected error when the host did not accept",
                            " * the topic, and with the bounds error when the message does not",
                            " * fit, sending nothing in each case. */",
                        ],
                        f"Publishes on {entry.name}. Fails with the not-connected error while "
                        "announcing, with the rejected error when the host did not accept "
                        "the topic, and with the bounds error when the message does not "
                        "fit, sending nothing in each case.",
                    ),
                    *_decl_lines(
                        "r2s_status_t",
                        f"r2s_publish_{entry.c_name}",
                        f"const {emit._c_name(entry.type_name)} *msg",
                        ";",
                    ),
                ]
            )
    for entry in table.entries:
        if entry.direction == "sub":
            lines.extend(
                [
                    "",
                    *_doc_lines(
                        [
                            f"/* Handles {entry.name}; the application implements it. Dispatch calls",
                            " * it from r2s_spin_some(), never from inside a publish. */",
                        ],
                        f"Handles {entry.name}; the application implements it. Dispatch calls "
                        "it from r2s_spin_some(), never from inside a publish.",
                    ),
                    *_decl_lines(
                        "void",
                        f"r2s_on_{entry.c_name}",
                        f"const {emit._c_name(entry.type_name)} *msg",
                        ";",
                    ),
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
                *_string_row("     ", entry.type_name, ","),
                *_string_row("     ", entry.name, "},"),
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
                    *_decl_lines(
                        "r2s_status_t",
                        f"r2s_publish_{entry.c_name}",
                        f"const {emit._c_name(entry.type_name)} *msg",
                        "",
                    ),
                    "{",
                    *_call_lines(
                        "    return r2s_publish_one(",
                        [
                            f"R2S_TOPIC_{entry.c_name.upper()}_ID",
                            (
                                "r2s_session_find_topic",
                                [f"R2S_TOPIC_{entry.c_name.upper()}_ID"],
                                "",
                            ),
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
            lines.extend(_member_lines("    ", emit._c_name(entry.type_name), entry.c_name))
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
                    *_assign_lines(
                        "        ",
                        "index",
                        "r2s_session_find_topic(",
                        f"R2S_TOPIC_{macro}_ID",
                        ");",
                    ),
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
                    *_call_lines(
                        f"        r2s_on_{entry.c_name}(",
                        [f"&r2s_rx_buf.{entry.c_name}"],
                        ");",
                    ),
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


def config_hash(loaded: Config) -> str:
    """16 hex characters over the canonical configuration, so a rename changes it."""
    lines = [
        loaded.node,
        loaded.distro,
        loaded.handshake,
        loaded.tier,
        " ".join(loaded.features),
    ]
    lines += [
        f"{topic.name} {topic.type_name} {topic.direction} {topic.qos} {int(topic.narrow_float64)}"
        for topic in loaded.topics
    ]
    lines += [f"{key} {loaded.bounds[key]}" for key in sorted(loaded.bounds)]
    lines += [str(loaded.default_string_bound), str(loaded.max_frame)]
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()[:16]


def banner(prefix: str, source: str, digest: str) -> str:
    """The file banner naming the generator version, the source config and its hash."""
    return f"{prefix} Generated by r2s-gen {__version__} from {source} (config {digest}); do not edit."


def _stamped(text: str, source: str, digest: str) -> str:
    """The emitted file with its banner naming the config it came from."""
    first, rest = text.split("\n", 1)
    assert first.startswith("/* Generated by r2s-gen ")
    return banner("/*", source, digest) + " */\n" + rest


def generate(
    config: Config, resolved: Mapping[str, ResolvedMessage], repository: MessageRepository, source: str = "r2s.yaml"
) -> tuple[str, str]:
    """The firmware's header and source pair for `config`, descriptors first."""
    roots = [canonical_type_name(topic.type_name) for topic in config.topics]
    table = descriptors.build(resolved, roots)
    topic_table = build(config, resolved, repository, table, source)
    digest = config_hash(config)
    base = _stamped(emit.emit_types(resolved, max_nest=table.max_nest), source, digest)
    closing = "#endif\n"
    assert base.endswith(closing)
    # A C++ sketch includes this header plainly: the guard below gives the
    # declarations C linkage, so no sketch writes its own extern "C" block.
    anchor = "#include <stdint.h>\n"
    assert base.count(anchor) == 1
    open = anchor + '\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
    close = "\n#ifdef __cplusplus\n}\n#endif\n"
    body = base[: -len(closing)] + emit_h(topic_table) + "\n" + close + "\n" + closing
    header = body.replace(anchor, open, 1)
    return header, _stamped(descriptors.emit_c(table) + emit_c(topic_table), source, digest)
