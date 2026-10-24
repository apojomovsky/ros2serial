"""Bounds resolution: give every variable-length field a concrete capacity or name it in an error.

Resolution walks the message graph from the generated topic types. A field takes
its bound from the config by `package/msg/Type.field`, a scalar string falls back
to `default_string_bound`, and anything left unbounded is an error that names the
field. Wide characters have no wire form here, so they are rejected the same way.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass

from r2s_gen.msgs import PRIMITIVES, WIDE_TYPES, Field, MessageError, MessageRepository, canonical_type_name


@dataclass(frozen=True)
class ResolvedField:
    name: str
    type_name: str
    is_array: bool
    is_sequence: bool
    array_bound: int | None
    string_bound: int | None


@dataclass(frozen=True)
class ResolvedMessage:
    type_name: str
    fields: tuple[ResolvedField, ...]

    def field(self, name: str) -> ResolvedField:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)


def _whole(value: object, where: str, least: int, what: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise MessageError(where, f"the {what} bound must be an integer", "give a whole number")
    if value < least:
        raise MessageError(where, f"the {what} bound must be at least {least}", "raise it")
    return value


def resolve(
    repository: MessageRepository,
    type_names: list[str] | tuple[str, ...],
    bounds: Mapping[str, object] | None = None,
    default_string_bound: int | None = None,
) -> dict[str, ResolvedMessage]:
    """The reachable messages from `type_names`, keyed by canonical type name, each field bounded."""
    configured = dict(bounds or {})
    if default_string_bound is not None:
        _whole(default_string_bound, "bounds.default_string_bound", 0, "string")
    resolved: dict[str, ResolvedMessage] = {}

    def visit(type_name: str, stack: tuple[str, ...]) -> None:
        if type_name in resolved:
            return
        if type_name in stack:
            raise MessageError(type_name, "recursive message type", "break the cycle")
        message = repository.load(type_name)
        fields = tuple(resolve_field(field, message.type_name, stack + (type_name,)) for field in message.fields)
        resolved[type_name] = ResolvedMessage(message.type_name, fields)

    def resolve_field(field: Field, owner: str, stack: tuple[str, ...]) -> ResolvedField:
        where = f"{owner}.{field.name}"
        if field.type_name in WIDE_TYPES:
            fix = "use a bounded string" if field.type_name == "wstring" else "use an 8-bit integer"
            raise MessageError(where, f"{field.type_name} is not supported", fix)
        sequence_unbounded = field.is_array and field.array_length is None and field.array_bound is None
        string_unbounded = field.type_name == "string" and field.string_bound is None
        array_bound = field.array_length if field.array_length is not None else field.array_bound
        string_bound = field.string_bound
        configured_bound = configured.get(where)
        if configured_bound is not None:
            if isinstance(configured_bound, list):
                if not (field.is_array and field.type_name == "string"):
                    raise MessageError(
                        where,
                        "a list bound is [count, string_bound], which needs a sequence of strings",
                        "use an integer bound instead",
                    )
                if len(configured_bound) != 2:
                    raise MessageError(
                        where, "a list bound is [count, string_bound]", "give the count and the string bound"
                    )
                count = _whole(configured_bound[0], where, 1, "sequence")
                element = _whole(configured_bound[1], where, 0, "string")
                if sequence_unbounded:
                    array_bound = count
                elif array_bound != count:
                    raise MessageError(where, "the sequence length is already bounded", "remove the count")
                if not string_unbounded:
                    raise MessageError(where, "the field is already bounded", "remove the bound")
                string_bound = element
            elif sequence_unbounded:
                array_bound = _whole(configured_bound, where, 1, "sequence")
            elif string_unbounded:
                string_bound = _whole(configured_bound, where, 0, "string")
            else:
                variable = field.is_array or field.type_name == "string"
                problem = "the field is already bounded" if variable else "the field has no variable length"
                raise MessageError(where, problem, "remove the bound")
        if sequence_unbounded and array_bound is None:
            raise MessageError(where, "unbounded sequence", f"set bounds['{where}'] to its element count")
        if string_unbounded:
            if string_bound is None:
                string_bound = default_string_bound
            if string_bound is None:
                fix = (
                    f"set bounds['{where}'] to [count, string_bound] or bounds.default_string_bound"
                    if field.is_array
                    else f"set bounds['{where}'] or bounds.default_string_bound"
                )
                raise MessageError(where, "unbounded string", fix)
        if field.type_name not in PRIMITIVES:
            visit(field.type_name, stack)
        return ResolvedField(
            name=field.name,
            type_name=field.type_name,
            is_array=field.is_array,
            is_sequence=field.is_array and field.array_length is None,
            array_bound=array_bound,
            string_bound=string_bound,
        )

    for type_name in type_names:
        visit(canonical_type_name(type_name), ())
    return resolved
