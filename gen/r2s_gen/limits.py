# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The `limits` subcommand: every bounded field of the built-in types, and its source.

A built-in type's capacity is somebody's choice, so "what does this field hold,
and who decided that" has to be one command. Each row is the storage
`bounds.resolve` already produced for the bundled library, labelled with the
mapping that supplied the capacity or with the definition when it bounded
itself. No second table exists to drift from the first.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from typing import TextIO

from r2s_gen import bundle
from r2s_gen.bundle import Library
from r2s_gen.msgs import MessageError

HEADER = ('type', 'field', 'kind', 'limit', 'source')
DEFINITION = '.msg'


@dataclass(frozen=True)
class Row:
    """One bounded field, spelled the way the generated struct holds it."""

    type_name: str
    field: str
    kind: str
    limit: str
    source: str


def rows(library: Library, source_map: str) -> tuple[Row, ...]:
    """
    Collect one row per bounded field, sorted by type and field name.

    :param library: the resolved library to read.
    :param source_map: the name a capacity from `library.bounds` is reported by.
    :return: the rows, in table order.
    """
    found = []
    for type_name in sorted(library.resolved):
        for field in library.resolved[type_name].fields:
            kind, limit = _storage(field)
            if kind is None:
                continue
            key = f'{type_name}.{field.name}'
            source = source_map if key in library.bounds else DEFINITION
            found.append(Row(type_name, field.name, kind, limit, source))
    return tuple(found)


def table(distro: str = 'jazzy') -> str:
    """
    Render the bundled library's table for `distro`, as the command prints it.

    :param distro: the snapshot distro to resolve from.
    :return: the table text, ending with a newline.
    :raises MessageError: if the distro is unknown.
    """
    rows_text = render(rows(bundle.bundled_library(distro), 'BUNDLE_BOUNDS'))
    return f'# bundled types, distro {distro}\n{rows_text}'


def render(found: tuple[Row, ...]) -> str:
    """
    Render rows as aligned columns under the five names each row carries.

    :param found: the rows to render.
    :return: the table text, ending with a newline.
    """
    cells = [HEADER]
    cells += [(row.type_name, row.field, row.kind, row.limit, row.source) for row in found]
    widths = [max(len(cell[index]) for cell in cells) for index in range(len(HEADER))]
    lines = [
        '  '.join(part.ljust(width) for part, width in zip(cell, widths, strict=True))
        for cell in cells
    ]
    return '\n'.join(line.rstrip() for line in lines) + '\n'


def _storage(field) -> tuple[str | None, str]:
    """Spell one field's storage, or nothing when the field has a fixed length."""
    if field.string_bound is not None:
        if field.is_array:
            return 'string array', f'{field.array_bound} x {field.string_bound}'
        return 'string', str(field.string_bound)
    if field.is_sequence:
        return 'sequence', str(field.array_bound)
    if field.is_array:
        return 'array', str(field.array_bound)
    return None, ''


def add_parser(subparsers: argparse._SubParsersAction) -> argparse.ArgumentParser:
    """Add the `limits` subcommand to the command line."""
    parser = subparsers.add_parser(
        'limits',
        help="print the built-in types' field limits and where each came from",
        description='Print every bounded field the bundled library carries, with the '
        'capacity it resolves to and whether BUNDLE_BOUNDS or the definition supplied it.',
    )
    parser.add_argument(
        '--distro',
        default='jazzy',
        help='the snapshot distro: jazzy, lyrical or rolling (default jazzy)',
    )
    parser.set_defaults(run=run)
    return parser


def run(args: argparse.Namespace) -> int:
    """Print the table for the distro; a bad input is exit 1 with its message."""
    try:
        text = table(args.distro)
    except MessageError as error:
        print(f'r2s-gen: error: {error}', file=sys.stderr)
        return 1
    stream: TextIO = sys.stdout
    stream.write(text)
    return 0
