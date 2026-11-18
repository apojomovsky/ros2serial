# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
Bounds resolution: every variable-length field gains a capacity or an error.

Resolution walks the message graph from the generated topic types. A field takes
its bound from its `.msg` definition or from the config by `package/msg/Type.field`,
and anything left unbounded is an error that names the field. A configured bound
on a field the `.msg` already bounds is an error too, because a smaller MCU buffer
cannot hold a message the host may legally send. Wide characters have no wire form
here, so they are rejected the same way.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from r2s_gen.docs import BOUNDS_URL
from r2s_gen.msgs import PRIMITIVES
from r2s_gen.msgs import WIDE_TYPES
from r2s_gen.msgs import Field
from r2s_gen.msgs import MessageError
from r2s_gen.msgs import MessageRepository
from r2s_gen.msgs import canonical_type_name


class BoundsError(MessageError):
    """
    A refused bound, pointing at the page that states the rule.

    Every bound a reader cannot resolve lands here, so the message carries the
    one page that says where a bound comes from and how to change it.
    """

    def __init__(self, where: str, problem: str, fix: str):
        """
        Record the refusal, with the section named after the fix.

        :param where: the field, type or flag the bound belongs to.
        :param problem: what is wrong.
        :param fix: what to do about it.
        """
        super().__init__(where, problem, f'{fix}; see {BOUNDS_URL}')


@dataclass(frozen=True)
class ResolvedField:
    """One field with every variable length replaced by a concrete bound."""

    name: str
    type_name: str
    is_array: bool
    is_sequence: bool
    array_bound: int | None
    string_bound: int | None


@dataclass(frozen=True)
class ResolvedMessage:
    """One message with every field resolved to a concrete bound."""

    type_name: str
    fields: tuple[ResolvedField, ...]

    def field(self, name: str) -> ResolvedField:
        """
        Return the resolved field called `name`.

        :param name: the field name to look up.
        :return: the resolved field.
        :raises KeyError: if no field is called `name`.
        """
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)


def _whole(value: object, where: str, least: int, what: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BoundsError(where, f'the {what} bound must be an integer', 'give a whole number')
    if value < least:
        raise BoundsError(where, f'the {what} bound must be at least {least}', 'raise it')
    return value


def resolve(
    repository: MessageRepository,
    type_names: list[str] | tuple[str, ...],
    bounds: Mapping[str, object] | None = None,
) -> dict[str, ResolvedMessage]:
    """
    Collect the reachable messages from `type_names`, keyed by canonical name.

    :param repository: the messages to resolve from.
    :param type_names: the entry-point types to walk from.
    :param bounds: per-field capacities from the config, if any.
    :return: each reachable message with every field bounded.
    :raises MessageError: if a field stays unbounded or breaks a rule above.
    """
    configured = dict(bounds or {})
    resolved: dict[str, ResolvedMessage] = {}

    def visit(type_name: str, stack: tuple[str, ...]) -> None:
        if type_name in resolved:
            return
        if type_name in stack:
            raise MessageError(type_name, 'recursive message type', 'break the cycle')
        message = repository.load(type_name)
        fields = tuple(
            resolve_field(field, message.type_name, message.path, stack + (type_name,))
            for field in message.fields
        )
        resolved[type_name] = ResolvedMessage(message.type_name, fields)

    def resolve_field(
        field: Field, owner: str, path: Path, stack: tuple[str, ...]
    ) -> ResolvedField:
        where = f'{owner}.{field.name}'
        if field.type_name in WIDE_TYPES:
            fix = (
                'use a bounded string' if field.type_name == 'wstring' else 'use an 8-bit integer'
            )
            raise BoundsError(where, f'{field.type_name} is not supported', fix)
        sequence_unbounded = (
            field.is_array and field.array_length is None and field.array_bound is None
        )
        string_unbounded = field.type_name == 'string' and field.string_bound is None
        array_bound = field.array_length if field.array_length is not None else field.array_bound
        string_bound = field.string_bound
        configured_bound = configured.get(where)
        if configured_bound is not None:
            if isinstance(configured_bound, list):
                if not (field.is_array and field.type_name == 'string'):
                    raise BoundsError(
                        where,
                        'a list bound is [count, string_bound], which needs a sequence of strings',
                        'use an integer bound instead',
                    )
                if len(configured_bound) != 2:
                    raise BoundsError(
                        where,
                        'a list bound is [count, string_bound]',
                        'give the count and the string bound',
                    )
                count = _whole(configured_bound[0], where, 1, 'sequence')
                element = _whole(configured_bound[1], where, 0, 'string')
                if sequence_unbounded:
                    array_bound = count
                elif array_bound != count:
                    raise BoundsError(
                        where,
                        f'the sequence length is already bounded in {path}',
                        'remove the count',
                    )
                if not string_unbounded:
                    raise BoundsError(
                        where, f'the field is already bounded in {path}', 'remove the bound'
                    )
                string_bound = element
            elif sequence_unbounded:
                array_bound = _whole(configured_bound, where, 1, 'sequence')
            elif string_unbounded:
                string_bound = _whole(configured_bound, where, 0, 'string')
            else:
                variable = field.is_array or field.type_name == 'string'
                problem = (
                    f'the field is already bounded in {path}'
                    if variable
                    else 'the field has no variable length'
                )
                raise BoundsError(where, problem, 'remove the bound')
        if sequence_unbounded and array_bound is None:
            raise BoundsError(
                where, 'unbounded sequence', f"set bounds['{where}'] to its element count"
            )
        if string_unbounded and string_bound is None:
            fix = (
                f"set bounds['{where}'] to [count, string_bound]"
                if field.is_array
                else f"set bounds['{where}']"
            )
            raise BoundsError(where, 'unbounded string', fix)
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
