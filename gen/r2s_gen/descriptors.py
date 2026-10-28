"""C emission: the descriptor tables a firmware holds for its message types.

`build` turns resolved types into the two tables of spec/protocol.md, section 9:
one record per type, one entry per field, nested types numbered before the types
that contain them. Sizes and offsets stay C expressions over the structs
`emit` generates, so one file describes every build's own structs (DSC-5); every
other number is a literal. `emit_c` renders the tables as a C file, in flash
through R2S_FLASH, with a compile-time check per field that its storage lies
inside its type (DSC-48). The nesting bound travels in the generated header,
where the codec reads it; the exact struct size is target-specific, so an
overflow the lower bound cannot prove surfaces as the emitted check failing.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from r2s_gen.bounds import ResolvedField, ResolvedMessage
from r2s_gen.emit import _c_name
from r2s_gen.msgs import MessageError, canonical_type_name
from r2s_gen.version import __version__

KINDS = {
    "bool": 0,
    "byte": 1,
    "char": 1,
    "uint8": 1,
    "int8": 2,
    "uint16": 3,
    "int16": 4,
    "uint32": 5,
    "int32": 6,
    "uint64": 7,
    "int64": 8,
    "float32": 9,
    "float64": 10,
    "string": 11,
}

_ELEMENT_BYTES = {
    "bool": 1,
    "byte": 1,
    "char": 1,
    "uint8": 1,
    "int8": 1,
    "uint16": 2,
    "int16": 2,
    "uint32": 4,
    "int32": 4,
    "float32": 4,
    "uint64": 8,
    "int64": 8,
    "float64": 8,
}


@dataclass(frozen=True)
class FieldEntry:
    owner: str
    field: str
    kind: int
    mode: int
    offset: str
    count: int
    aux: int


@dataclass(frozen=True)
class TypeRecord:
    type_name: str
    size: str
    first_field: int
    field_count: int


@dataclass(frozen=True)
class DescriptorTable:
    records: tuple[TypeRecord, ...]
    entries: tuple[FieldEntry, ...]
    index_of: dict[str, int]
    max_nest: int


def build(resolved: Mapping[str, ResolvedMessage], roots: Sequence[str]) -> DescriptorTable:
    """The descriptor table for `roots` in configuration order, validated as the loader rules require."""
    names = [canonical_type_name(root) for root in roots]
    for name in names:
        if name not in resolved:
            raise MessageError(name, "unknown message type", "resolve it before building descriptors")
    ordered = _ordered(resolved, names)
    index_of = {name: position for position, name in enumerate(ordered)}
    entries: list[FieldEntry] = []
    records: list[TypeRecord] = []
    first = 0
    for name in ordered:
        message = resolved[name]
        if len(message.fields) > 255:
            raise MessageError(name, "breaks DSC-22: more than 255 fields have no descriptor form", "split the type")
        for field in message.fields:
            entries.append(_entry(resolved, index_of, name, field))
        records.append(
            TypeRecord(
                type_name=name, size=f"sizeof({_c_name(name)})", first_field=first, field_count=len(message.fields)
            )
        )
        first += len(message.fields)
    if len(entries) > 65535:
        raise MessageError(
            names[0], "breaks DSC-22: more than 65535 field entries have no descriptor form", "use fewer types"
        )
    for name in ordered:
        if _minimum_size(resolved, name, {}) > 65535:
            raise MessageError(
                name, "breaks DSC-22: a struct over 65535 bytes has no descriptor form", "shrink the type"
            )
    depths = [_depth(resolved, name, {}) for name in ordered]
    return DescriptorTable(
        records=tuple(records), entries=tuple(entries), index_of=index_of, max_nest=max([1, *depths])
    )


def _ordered(resolved: Mapping[str, ResolvedMessage], roots: list[str]) -> list[str]:
    """Every reachable type once, each nested type before the types that contain it."""
    ordered: list[str] = []
    visiting: set[str] = set()
    done: set[str] = set()

    def visit(name: str) -> None:
        if name in done:
            return
        if name in visiting:
            raise MessageError(name, "breaks DSC-12: recursive types have no descriptor form", "break the cycle")
        if name not in resolved:
            raise MessageError(name, f"unknown message type '{name}'", "resolve it before building descriptors")
        visiting.add(name)
        for field in resolved[name].fields:
            if field.type_name in resolved:
                visit(field.type_name)
            elif field.type_name not in KINDS:
                raise MessageError(
                    f"{name}.{field.name}",
                    f"unknown field type '{field.type_name}'",
                    "use a primitive or a resolved message",
                )
        visiting.remove(name)
        done.add(name)
        ordered.append(name)

    for root in roots:
        visit(root)
    return ordered


def _entry(
    resolved: Mapping[str, ResolvedMessage], index_of: dict[str, int], owner: str, field: ResolvedField
) -> FieldEntry:
    """The field entry for `field`: kinds, modes, counts and bounds are literals, the offset a C expression."""
    name = _c_name(owner)
    if field.type_name in resolved:
        kind = 12
        aux = index_of[field.type_name]
    elif field.type_name == "string":
        kind = 11
        if field.string_bound is None:
            raise MessageError(
                f"{owner}.{field.name}", "breaks DSC-22: an unbounded string has no descriptor form", "bound the string"
            )
        aux = field.string_bound
    else:
        kind = KINDS[field.type_name]
        aux = 0
    if not field.is_array:
        if field.array_bound is not None:
            raise MessageError(
                f"{owner}.{field.name}",
                "breaks DSC-44: a scalar's count is 0, so it takes no bound",
                "remove the bound",
            )
        mode, count = 0, 0
        offset = f"offsetof({name}, {field.name})"
    else:
        if field.is_sequence and field.array_bound is None:
            raise MessageError(
                f"{owner}.{field.name}",
                "breaks DSC-22: an unbounded sequence has no descriptor form",
                "bound the sequence",
            )
        if not field.array_bound:
            raise MessageError(
                f"{owner}.{field.name}", "breaks DSC-44: an array's count is its length, never 0", "bound the array"
            )
        wide = field.is_sequence and field.array_bound > 255
        mode = 1 if not field.is_sequence else (2 | (0x80 if wide else 0))
        count = field.array_bound
        offset = f"offsetof({name}, {field.name}.data)" if field.is_sequence else f"offsetof({name}, {field.name})"
    return FieldEntry(owner=owner, field=field.name, kind=kind, mode=mode, offset=offset, count=count, aux=aux)


def _minimum_size(resolved: Mapping[str, ResolvedMessage], name: str, memo: dict[str, int]) -> int:
    """A lower bound of the struct's size: padding only adds, so over 65535 the type never fits."""
    if name in memo:
        return memo[name]
    total = 0
    for field in resolved[name].fields:
        if field.type_name in resolved:
            element = _minimum_size(resolved, field.type_name, memo)
        elif field.type_name == "string":
            element = (field.string_bound or 0) + 1
        else:
            element = _ELEMENT_BYTES[field.type_name]
        if not field.is_array:
            total += element
        elif not field.is_sequence:
            total += (field.array_bound or 0) * element
        else:
            total += (field.array_bound or 0) * element + (2 if (field.array_bound or 0) > 255 else 1)
    memo[name] = max(total, 1)
    return memo[name]


def _depth(resolved: Mapping[str, ResolvedMessage], name: str, memo: dict[str, int]) -> int:
    """The nesting depth of `name`: 1 past the deepest nested type it fields, arrays adding no level."""
    if name in memo:
        return memo[name]
    nested = [f.type_name for f in resolved[name].fields if f.type_name in resolved]
    memo[name] = 1 + max([_depth(resolved, nested_name, memo) for nested_name in nested] or [0])
    return memo[name]


# clang-format aligns a macro's escaped newlines at one column; the emitter
# reproduces that column, so its output is already formatted.
_BACKSLASH_COLUMN = 109

_FIELD_CHECK_DEFINE = [
    "#define R2S_FIELD_CHECK(type, field, end)".ljust(_BACKSLASH_COLUMN) + "\\",
    "    typedef char r2s_dsc48_storage_check_##type##__##field".ljust(_BACKSLASH_COLUMN) + "\\",
    "        [((uint32_t)(offsetof(type, end) + sizeof(((type *)0)->end)) <= (uint32_t)(sizeof(type))) ? 1 : -1]",
]


def record_lines(table: DescriptorTable) -> list[str]:
    """One record's five bytes per type, in the layout `r2s_codec_table_t` walks."""
    lines = []
    for record in table.records:
        c_name = _c_name(record.type_name)
        lines.append(f"    R2S_U16_LO(sizeof({c_name})),")
        lines.append(f"    R2S_U16_HI(sizeof({c_name})),")
        lines.append(f"    0x{record.first_field & 0xFF:02x},")
        lines.append(f"    0x{(record.first_field >> 8) & 0xFF:02x},")
        lines.append(f"    0x{record.field_count:02x}, /* {record.type_name} */")
    return lines


def entry_lines(table: DescriptorTable) -> list[str]:
    """One entry's eight bytes per field, in the layout `r2s_codec_table_t` walks."""
    lines = []
    for entry in table.entries:
        lines.append(f"    0x{entry.kind:02x},")
        lines.append(f"    0x{entry.mode:02x},")
        lines.append(f"    R2S_U16_LO({entry.offset}),")
        lines.append(f"    R2S_U16_HI({entry.offset}),")
        lines.append(f"    0x{entry.count & 0xFF:02x},")
        lines.append(f"    0x{(entry.count >> 8) & 0xFF:02x},")
        lines.append(f"    0x{entry.aux & 0xFF:02x},")
        lines.append(f"    0x{(entry.aux >> 8) & 0xFF:02x}, /* {entry.owner}.{entry.field} */")
    return lines


def emit_c(table: DescriptorTable) -> str:
    """The C file holding the tables: records, entries, counts, the nesting bound and storage checks."""
    lines = [
        f"/* Generated by r2s-gen {__version__} from the resolved message types; do not edit. */",
        '#include "r2s_generated.h"',
        "",
        '#include "r2s/r2s_codec.h"',
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "/* Type records and field entries (spec/protocol.md, section 9), the tables the",
        " * codec interpreter walks. Sizes and offsets come from the structs above, so",
        " * one file describes every build; multi-byte integers are little-endian",
        " * (DSC-4). */",
        "",
        f"#define R2S_TYPE_COUNT {len(table.records)}u",
        f"#define R2S_FIELD_COUNT {len(table.entries)}u",
        "",
        "#define R2S_U16_LO(value) ((uint8_t)((value) & 0xffu))",
        "#define R2S_U16_HI(value) ((uint8_t)(((value) >> 8) & 0xffu))",
        "",
        "/* A type fits its record when its struct holds in 16 bits (DSC-22), and a field",
        " * lies inside its type when its storage ends at or before the struct end",
        " * (DSC-48). The sums run in 32 bits so they cannot wrap (DSC-21). */",
        "#define R2S_SIZE_CHECK(type) typedef char r2s_dsc22_size_check_##type[(sizeof(type) <= 0xffffu) ? 1 : -1]",
        "",
        *_FIELD_CHECK_DEFINE,
        "",
    ]
    for record in table.records:
        lines.append(f"R2S_SIZE_CHECK({_c_name(record.type_name)});")
    if table.records:
        lines.append("")
        lines.append("const uint8_t R2S_FLASH r2s_type_records[] = {")
        for record in table.records:
            c_name = _c_name(record.type_name)
            lines.append(f"    R2S_U16_LO(sizeof({c_name})),")
            lines.append(f"    R2S_U16_HI(sizeof({c_name})),")
            lines.append(f"    0x{record.first_field & 0xFF:02x},")
            lines.append(f"    0x{(record.first_field >> 8) & 0xFF:02x},")
            lines.append(f"    0x{record.field_count:02x}, /* {record.type_name} */")
        lines.append("};")
        lines.append("")
    else:
        lines.append("")
        lines.append("/* No types: one unread byte keeps the table symbol defined; the count stays 0. */")
        lines.append("const uint8_t R2S_FLASH r2s_type_records[1] = { 0x00 };")
        lines.append("")
    if table.entries:
        lines.append("const uint8_t R2S_FLASH r2s_field_entries[] = {")
        for entry in table.entries:
            lines.append(f"    0x{entry.kind:02x},")
            lines.append(f"    0x{entry.mode:02x},")
            lines.append(f"    R2S_U16_LO({entry.offset}),")
            lines.append(f"    R2S_U16_HI({entry.offset}),")
            lines.append(f"    0x{entry.count & 0xFF:02x},")
            lines.append(f"    0x{(entry.count >> 8) & 0xFF:02x},")
            lines.append(f"    0x{entry.aux & 0xFF:02x},")
            lines.append(f"    0x{(entry.aux >> 8) & 0xFF:02x}, /* {entry.owner}.{entry.field} */")
        lines.append("};")
        lines.append("")
    else:
        lines.append("/* No entries: one unread byte keeps the table symbol defined; the count stays 0. */")
        lines.append("const uint8_t R2S_FLASH r2s_field_entries[1] = { 0x00 };")
        lines.append("")
    for entry in table.entries:
        member = entry.offset.split(", ", 1)[1][:-1]
        if entry.mode in (2, 0x82):
            member = f"{entry.field}.size"
        lines.append(f"R2S_FIELD_CHECK({_c_name(entry.owner)}, {entry.field}, {member});")
    lines.append("")
    return "\n".join(lines)
