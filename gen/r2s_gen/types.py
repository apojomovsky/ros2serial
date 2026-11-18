# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The `types` subcommand: a library emitted for the user's own interface types.

The input is a type list, on the command line or in a file, and both spell the
bounds the same way: `package/{msg,srv,action}/Type.field`, as `bounds` in
`r2s.yaml` spells it. A service or an action resolves to the parts its
definition splits into, because a bound belongs to a part's field. Nothing here
is a fallback: a field the `.msg` does not bound and the input does not either
fails naming it, and a bound that contradicts the `.msg` fails naming both.

Emission is `bundle.emit_library`, so the bundled library and a user's are the
same code over a different `Library`. The digest covers the resolved type list,
the bounds and the distro, and the library name carries it: two libraries with
different type lists must never share a name, because Arduino picks one
silently when they do.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections.abc import Iterable
from collections.abc import Mapping
from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import Any

from r2s_gen import bounds as bounds_module
from r2s_gen import bundle
from r2s_gen import docs
from r2s_gen import layouts
from r2s_gen import msgs
from r2s_gen.bundle import Library
from r2s_gen.msgs import MessageError
from r2s_gen.version import __version__

FILE_NAME = 'r2s-types.yaml'

SERVICE_PARTS = ('_Request', '_Response')
ACTION_PARTS = ('_Goal', '_Result', '_Feedback')


@dataclass(frozen=True)
class Entry:
    """One type the library carries, with the bounds its own entry names."""

    type_name: str
    bounds: Mapping[str, object] = field(default_factory=dict)


def plan(
    entries: Iterable[Entry],
    *,
    distro: str = 'jazzy',
    msg_paths: Iterable[str] = (),
    source: str = 'the type list',
    flagged: Mapping[str, object] | None = None,
) -> Library:
    """
    Resolve a type list into the library its emission reads.

    :param entries: the types to carry, with their bounds.
    :param distro: the snapshot distro to resolve from.
    :param msg_paths: the message packages to search before the snapshot.
    :param source: what the banner names as the library's origin.
    :param flagged: bounds from the command line, keyed as the file keys them.
    :return: the resolved library.
    :raises MessageError: on an unbounded field or a contradictory bound.
    """
    if distro not in bundle.DISTROS:
        raise MessageError(
            '--distro', f"unknown distro '{distro}'", 'use one of: jazzy, lyrical, rolling'
        )
    entries = tuple(entries)
    if not entries:
        raise MessageError(source, 'no types', 'name at least one type, such as package/msg/Type')
    roots: list[str] = []
    actions: list[str] = []
    for entry in entries:
        canonical = msgs.canonical_type_name(entry.type_name)
        # A service or an action names the whole of itself and expands to its
        # parts; a name that spells one part alone is a definition that has to
        # arrive with its siblings, which _whole refuses otherwise.
        if '/srv/' in canonical and not canonical.endswith(SERVICE_PARTS):
            roots.extend(f'{canonical}{part}' for part in SERVICE_PARTS)
        elif '/action/' in canonical and not canonical.endswith(ACTION_PARTS):
            actions.append(canonical)
            roots.extend(f'{canonical}{part}' for part in ACTION_PARTS)
        else:
            roots.append(canonical)
    _whole(entries)
    repository = layouts.repository(list(msg_paths), distro)
    carried = _carried_fields(roots, repository)
    configured = _merge(source, entries, flagged or {}, carried)
    _check_carried(source, configured, carried)
    resolved = bounds_module.resolve(repository, roots, bounds=configured)
    types = tuple(sorted(roots))
    # The digest covers everything the emission keys on, so two libraries with
    # different content can never share a name: the parts, the action headers
    # that carry them, and the bounds.
    digest = _digest(types, actions, configured, distro)
    return Library(
        types=types,
        bounds=configured,
        distro=distro,
        resolved=resolved,
        repository=repository,
        origin=source,
        name=f'ros2serial_types_{digest}',
        digest=digest,
        sentence=f'ros2serial firmware types for {len(types)} interface types',
        kind='types',
        actions=tuple(sorted(actions)),
        nested=_nested(types, resolved),
        frame_checks=True,
    )


def emit(library: Library, layout: str) -> dict[str, str]:
    """
    Emit every file of a type library, the wrapper its layout reads included.

    :param library: the library to write.
    :param layout: `arduino`, `platformio` or `cmake`.
    :return: file name to file text, in build order.
    """
    name = wrapper_name(layout)
    files = bundle.emit_library(library)
    if layout == 'platformio':
        return {name: _library_json(library, files['src/r2s_bundled.h']), **files}
    return {name: Wrappers[layout](library), **files}


def files_for(library: Library, layout: str) -> tuple[str, ...]:
    """
    Return the relative paths a type library writes for `layout`, in build order.

    :param library: the library to write.
    :param layout: `arduino`, `platformio` or `cmake`.
    :return: the relative paths.
    :raises MessageError: if the layout is unknown.
    """
    return (wrapper_name(layout), *bundle.library_files(library))


def wrapper_name(layout: str) -> str:
    """
    Return the wrapper file `layout`'s toolchain reads.

    :param layout: the layout name.
    :return: the wrapper's file name.
    :raises MessageError: if the layout is unknown.
    """
    name = LAYOUT_FILES.get(layout)
    if name is None:
        raise MessageError(
            '--layout', f"unknown layout '{layout}'", 'use one of: arduino, platformio, cmake'
        )
    return name


def write_config(path: str | Path, library: Library) -> None:
    """
    Write the equivalent type list, so a one-off command becomes reviewable.

    A service and an action are written as themselves, with their bounds beside
    the part they belong to, so the file re-reads to the library the command
    emitted.

    :param path: the file to write.
    :param library: the library the command resolved.
    :raises OSError: if the file cannot be written.
    """
    bounds = library.bounds

    def own(name: str) -> dict[str, object]:
        return {k: v for k, v in bounds.items() if k.startswith(f'{name}.')}

    def entry(name: str) -> list[str]:
        keys = own(name)
        if not keys:
            return [f'  - {name}']
        return [f'  - type: {name}', '    bounds:'] + [
            f'      {key.split(".", 1)[1]}: {keys[key]}' for key in sorted(keys)
        ]

    lines = [
        '# The type list r2s-gen types resolved, so the same command can be',
        '# replayed from a file under review.',
        f'distro: {library.distro}',
        'types:',
    ]
    for name in _listed(library):
        lines.extend(entry(name))
    shared = {
        key: value
        for key, value in bounds.items()
        if not any(key.startswith(f'{name}.') for name in _listed(library))
    }
    if shared:
        lines.append('bounds:')
        lines.extend(f'  {key}: {shared[key]}' for key in sorted(shared))
    _write(Path(path), '\n'.join(lines) + '\n')


def _listed(library: Library) -> list[str]:
    """Return the entries that produce the library: its services, actions and plain types."""
    parts = tuple(f'{action}{part}' for action in library.actions for part in ACTION_PARTS)
    services = tuple(f'{service}{part}' for service in library.services for part in SERVICE_PARTS)
    plain = [name for name in library.types if name not in parts and name not in services]
    return sorted([*plain, *library.services, *library.actions])


LAYOUT_FILES = {
    'arduino': 'library.properties',
    'platformio': 'library.json',
    'cmake': 'CMakeLists.txt',
}


def _library_properties(library: Library) -> str:
    """Render the Arduino library metadata for the type library, AVR and ESP32."""
    lines = [
        f'# {bundle.banner(library)}',
        f'name={library.name}',
        f'version={__version__}',
        'author=ros2serial contributors',
        'maintainer=ros2serial contributors',
        f'sentence={library.sentence}',
        f'paragraph={library.paragraph}',
        'category=Communication',
        'url=https://github.com/apojomovsky/ros2serial',
        'architectures=avr,esp32',
        'includes=ros2serial.h',
        '',
    ]
    return '\n'.join(lines)


def _library_json(library: Library, header: str) -> str:
    """Render the PlatformIO manifest for the type library."""
    defines = layouts.build_defines(header, 'src/r2s_bundled.h')
    manifest = {
        'name': library.name,
        'version': __version__,
        'description': library.sentence,
        'keywords': 'ros2serial',
        'repository': {'type': 'git', 'url': 'https://github.com/apojomovsky/ros2serial'},
        'authors': [{'name': 'ros2serial contributors'}],
        'license': 'MIT',
        'frameworks': 'arduino',
        'platforms': ['atmelavr', 'espressif32'],
        'headers': 'ros2serial.h',
        'build': {'flags': [f'-D{define}' for define in defines]},
    }
    stamped = {'generated': bundle.banner(library), **manifest}
    return json.dumps(stamped, indent=4) + '\n'


def _cmake_lists(library: Library) -> str:
    """Render the CMake wrapper: the library's sources plus the core's."""
    lines = [
        f'# {bundle.banner(library)}',
        'cmake_minimum_required(VERSION 3.22)',
        f'project({library.name} C CXX)',
        '',
        'set(CMAKE_C_STANDARD 99)',
        'set(CMAKE_C_STANDARD_REQUIRED ON)',
        'set(CMAKE_C_EXTENSIONS OFF)',
        '',
        '# The portable core this library builds: pass -DR2S_CORE_DIR=<dir> naming',
        '# the checkout whose src/ holds the core sources and whose include/ holds r2s/.',
        'if(NOT R2S_CORE_DIR)',
        '  message(FATAL_ERROR "R2S_CORE_DIR names no core directory '
        '(pass -DR2S_CORE_DIR=<repo>/core)")',
        'endif()',
        'file(GLOB R2S_CORE_SOURCES CONFIGURE_DEPENDS "${R2S_CORE_DIR}/src/*.c")',
        'list(REMOVE_ITEM R2S_CORE_SOURCES "${R2S_CORE_DIR}/src/test_reset.c")',
        '',
        f'add_library({library.name} STATIC ${{R2S_CORE_SOURCES}} src/r2s_bundled.c)',
        f'target_include_directories({library.name} PUBLIC "${{R2S_CORE_DIR}}/include" '
        '"${CMAKE_CURRENT_SOURCE_DIR}/src")',
        *layouts.limit_defines(
            'src/r2s_bundled.h',
            library.name,
            [
                '    -pedantic -Wall -Wextra -Werror -Wconversion -Wshadow',
                '    -Wstrict-prototypes -Wmissing-prototypes -Wundef',
            ],
        ),
        '',
    ]
    return '\n'.join(lines)


Wrappers = {
    'arduino': _library_properties,
    'cmake': _cmake_lists,
}


def _merge(
    source: str,
    entries: Iterable[Entry],
    flagged: Mapping[str, object],
    fields: Mapping[str, set],
) -> dict:
    """
    Collect the entry bounds and the flagged ones, refusing a key set two ways.

    An entry's bound key is the field name alone for a message, and the part
    and the field for a service or an action, since a part is what holds the
    field. A flag always spells the whole path. Both land here as the whole
    path the resolver keys on.
    """
    merged: dict[str, object] = {}
    for entry, bounds in [(entry, entry.bounds) for entry in entries] + [
        (Entry('', flagged), flagged)
    ]:
        for key, value in _qualified(entry.type_name, bounds, fields).items():
            if key in merged and merged[key] != value:
                raise MessageError(
                    f'{source}: bounds',
                    f"'{key}' is set to {merged[key]} and to {value}",
                    'keep one value',
                )
            merged[key] = value
    return merged


def _qualified(type_name: str, bounds: Mapping[str, object], fields: Mapping[str, set]) -> dict:
    """Spell every short bound key as the whole field path the resolver reads."""
    if not type_name:
        return dict(bounds)
    canonical = msgs.canonical_type_name(type_name)
    out: dict[str, object] = {}
    for key, value in bounds.items():
        if '/' in key and '.' in key:
            out[key] = value
            continue
        owner = canonical
        if '/srv/' in canonical and not canonical.endswith(SERVICE_PARTS):
            owner = _part(canonical, key, SERVICE_PARTS, fields)
        elif '/action/' in canonical and not canonical.endswith(ACTION_PARTS):
            owner = _part(canonical, key, ACTION_PARTS, fields)
        out[f'{owner}.{key.rsplit(".", 1)[-1]}'] = value
    return out


def _part(canonical: str, key: str, parts: Iterable[str], fields: Mapping[str, set]) -> str:
    """
    Return the part a short key's field belongs to.

    `result.note` names the part outright. `note` leaves it to the definitions:
    exactly one part holding a field of that name is the part meant, and a
    name two parts share is refused rather than guessed.
    """
    head = key.split('.', 1)[0]
    for part in parts:
        if head in (part[1:].lower(), part[1:], part):
            return f'{canonical}{part}'
    owners = [part for part in parts if key in fields.get(f'{canonical}{part}', set())]
    if len(owners) == 1:
        return f'{canonical}{owners[0]}'
    named = ', '.join(part[1:].lower() for part in parts)
    raise MessageError(
        f'{canonical}: bounds',
        f"'{key}' names no part" if not owners else f"'{key}' is in more than one part",
        f'name one of {named} and its field, as result.note',
    )


def _carried_fields(roots: Iterable[str], repository: msgs.MessageRepository) -> dict[str, set]:
    """Collect every field the library carries, from the closure of its roots."""
    carried: dict[str, set] = {}
    pending = list(roots)
    while pending:
        name = pending.pop()
        if name in carried:
            continue
        message = repository.load(name)
        carried[name] = {field.name for field in message.fields}
        pending += [
            field.type_name
            for field in message.fields
            if field.type_name not in carried and '/' in field.type_name
        ]
    return carried


def _check_carried(
    source: str, configured: Mapping[str, object], carried: Mapping[str, set]
) -> None:
    """Refuse a bound on a field the library does not carry, naming both."""
    for key in sorted(configured):
        owner, _, field = key.rpartition('.')
        if owner not in carried or field not in carried[owner]:
            raise MessageError(
                f'{source}: bounds',
                f"'{key}' is not a field of a type this library carries",
                'fix the field path, or add the type',
            )


def _digest(
    types: Iterable[str], actions: Iterable[str], bounds: Mapping[str, object], distro: str
) -> str:
    """16 hex characters over the resolved types, their actions, their bounds and the distro."""
    lines = [distro, *sorted(types)]
    # The action headers ship an entity the parts do not name on their own, and
    # the part indices differ between the two spellings, so the actions join
    # the digest: an action and its bare parts must not share a name.
    lines += [f'action {name}' for name in sorted(actions)]
    lines += [f'{key} {bounds[key]}' for key in sorted(bounds)]
    return hashlib.sha256('\n'.join(lines).encode('utf-8')).hexdigest()[:16]


def _whole(entries: Iterable[Entry]) -> None:
    """Refuse a part named alone: its siblings are what its definition needs."""
    named = {msgs.canonical_type_name(entry.type_name) for entry in entries}
    for name in sorted(named):
        for suffix, parts in (('/srv/', SERVICE_PARTS), ('/action/', ACTION_PARTS)):
            if suffix not in name:
                continue
            base, part = _split_part(name, parts)
            if part is None:
                continue
            missing = [f'{base}{other}' for other in parts if f'{base}{other}' not in named]
            if missing:
                raise MessageError(
                    name,
                    f'the part arrives without {missing[0]}',
                    f'name {base} instead, which carries all its parts',
                )


def _split_part(name: str, parts: Iterable[str]) -> tuple[str, str | None]:
    """Split a part name into its definition and the part, or the name and nothing."""
    for part in parts:
        if name.endswith(part):
            return name[: -len(part)], part
    return name, None


def _nested(types: Iterable[str], resolved: Mapping[str, object]) -> tuple[str, ...]:
    """Return the closure types the library ships headers for beyond its own list."""
    return tuple(sorted(set(resolved) - set(types)))


def read_list(path: str | Path) -> tuple[Entry, ...]:
    """
    Read a type list file: `types` entries plus an optional library-wide `bounds`.

    :param path: the file to read.
    :return: one entry per type, with the library-wide bounds folded in.
    :raises MessageError: if the file cannot be read or breaks its shape.
    """
    source = str(path)
    data = _parse(source, _read(source))
    listed = data.get('types')
    if listed is None:
        raise MessageError(source, "missing required key 'types'", 'add types: with one per line')
    if not isinstance(listed, list):
        raise MessageError(source, "'types' must be a list", 'write - package/msg/Type per line')
    for key in data:
        if key not in ('types', 'bounds', 'distro'):
            raise MessageError(f'{source}: {key}', 'unknown key', 'use types, bounds or distro')
    shared = _bounds(source, data.get('bounds'))
    entries: list[Entry] = []
    for index, item in enumerate(listed):
        where = f'{source}: types[{index}]'
        if isinstance(item, str):
            entries.append(Entry(item, dict(shared)))
            continue
        if not isinstance(item, dict):
            raise MessageError(where, 'expected a type or a mapping', 'write - package/msg/Type')
        unknown = sorted(set(item) - {'type', 'bounds'})
        if unknown:
            raise MessageError(
                where, f"unknown key '{unknown[0]}'", 'write type: and bounds: only'
            )
        if 'type' not in item or not isinstance(item['type'], str):
            raise MessageError(where, "missing required key 'type'", 'add type: package/msg/Type')
        own = _bounds(where, item.get('bounds'))
        entries.append(Entry(item['type'], {**shared, **own}))
    return tuple(entries)


def file_distro(path: str | Path) -> str | None:
    """Return the distro a type list file pins, if it pins one."""
    value = _parse(str(path), _read(path)).get('distro')
    return value if isinstance(value, str) else None


def _bounds(where: str, value: Any) -> dict[str, object]:
    """
    Read one `bounds` mapping.

    A key names a field, as `text` for a message entry, `result.note` for an
    action entry, or the whole `package/msg/Type.field` path for the block a
    file carries outside its entries. `_qualified` finishes the short forms.
    """
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise MessageError(where, "'bounds' must be a mapping", 'write one field path per line')
    for key in value:
        if not isinstance(key, str) or not key:
            raise MessageError(
                f'{where}: bounds',
                f"'{key}' is not a field path",
                'write Type.field, or package/msg/Type.field',
            )
    return dict(value)


def _read(path: str | Path) -> str:
    try:
        return Path(path).read_text(encoding='utf-8')
    except UnicodeDecodeError as error:
        raise MessageError(
            str(path), f'cannot read the type list ({error})', 'save the file as UTF-8'
        ) from error
    except OSError as error:
        raise MessageError(
            str(path), f'cannot read the type list ({error})', 'check the path'
        ) from error


def _parse(source: str, raw: str) -> dict:
    try:
        import yaml
    except ImportError as error:
        raise MessageError(
            source,
            'cannot read the type list without PyYAML',
            'install it with pip install pyyaml',
        ) from error
    try:
        data = yaml.safe_load(raw)
    except yaml.YAMLError as error:
        raise MessageError(source, 'not valid YAML', 'fix the indentation and quoting') from error
    if not isinstance(data, dict):
        raise MessageError(source, 'the type list must be a mapping', 'start the file with types:')
    return data


def _write(path: Path, text: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    except OSError as error:
        raise OSError(f'cannot write {path} ({error}); check the path') from error


def parse_bound(flag: str) -> tuple[str, object]:
    """
    Read one `--bound package/msg/Type.field=N` value.

    :param flag: the argument's value, without the flag itself.
    :return: the field path and its value, an int or a `[count, string]` pair.
    :raises MessageError: if the value is not a bound.
    """
    key, separator, text = flag.partition('=')
    if not separator or not key or not text.strip():
        raise MessageError(
            f'--bound {flag}', 'expected TYPE.FIELD=N', 'write --bound std_msgs/msg/String.data=24'
        )
    value = text.strip()
    try:
        if value.startswith('['):
            left, _, right = value.strip('[]').partition(',')
            parsed: object = [int(left.strip()), int(right.strip())]
        else:
            parsed = int(value)
    except ValueError:
        raise MessageError(
            f'--bound {flag}', f"'{text}' is not a bound", 'give a number, or [count, string]'
        ) from None
    return key, parsed


def _flagged(flags: Iterable[str]) -> dict[str, object]:
    """Read every `--bound` value, refusing one field set two different ways."""
    flagged: dict[str, object] = {}
    for flag in flags:
        key, value = parse_bound(flag)
        if key in flagged and flagged[key] != value:
            raise MessageError(
                f'--bound {flag}',
                f"'{key}' is already bounded to {flagged[key]}",
                'keep one value',
            )
        flagged[key] = value
    return flagged


def add_parser(subparsers: argparse._SubParsersAction) -> argparse.ArgumentParser:
    """Add the `types` subcommand to the command line."""
    parser = subparsers.add_parser(
        'types',
        help="write the type library the user's interface types need",
        description='Write an Arduino-style library for the given interface types, consumed '
        'by the X-macro and runtime routes unchanged, under the out directory.',
    )
    parser.add_argument(
        'types', nargs='*', metavar='TYPE', help='package/msg/Type, or package/srv/Name'
    )
    parser.add_argument('--types-file', help=f'the type list file, conventionally {FILE_NAME}')
    parser.add_argument(
        '--msg-path',
        action='append',
        default=[],
        metavar='DIR',
        help='search DIR for message packages; repeatable',
    )
    parser.add_argument(
        '--bound',
        action='append',
        default=[],
        metavar='TYPE.FIELD=N',
        help='bound one variable-length field, as package/msg/Type.field=N; repeatable',
    )
    parser.add_argument(
        '--write-config',
        metavar='FILE',
        help='write the equivalent type list, so a one-off command becomes reviewable',
    )
    parser.add_argument(
        '--layout',
        default='arduino',
        help='the wrapper to emit: arduino, platformio or cmake (default arduino)',
    )
    parser.add_argument('--distro', help='the snapshot distro: jazzy, lyrical or rolling')
    parser.add_argument('--out', required=True, help='the directory receiving the library files')
    parser.set_defaults(run=run)
    return docs.note(parser)


def run(args: argparse.Namespace) -> int:
    """Write the type library; a bad input is exit 1 with its message."""
    try:
        entries = list(read_list(args.types_file)) if args.types_file else []
        entries += [Entry(name) for name in args.types]
        flagged = _flagged(args.bound)
        library = plan(
            entries,
            distro=_distro(args.distro, args.types_file),
            msg_paths=args.msg_path,
            source=args.types_file or 'the command line',
            flagged=flagged,
        )
        files = emit(library, args.layout)
        if args.write_config:
            write_config(args.write_config, library)
        layouts._write(args.out, files)
    except (MessageError, OSError) as error:
        print(f'r2s-gen: error: {error}', file=sys.stderr)
        return 1
    for name in files:
        print(name)
    return 0


def _distro(flag: str | None, types_file: str | None) -> str:
    """Resolve the distro: the flag, then the file, then the environment, then the default."""
    if flag:
        return flag
    if types_file:
        pinned = file_distro(types_file)
        if pinned:
            return pinned
    return layouts.sourced_distro() or bundle.DISTROS[0]
