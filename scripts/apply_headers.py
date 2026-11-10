#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
"""Apply the SPDX licence header across the tree, replacing the old MIT block.

The tool walks the same scope the gate checks (check_headers.in_scope, with
its generated and vendored exemptions), replaces a leading old MIT block with
the SPDX two-liner, and inserts the header where one is missing. Files named
on the command line take the replacement path even outside the scope, so the
gate's own scripts convert the same way; insertion stays scoped, so the
licence file and byte-sensitive fixtures never gain a header. A second run
changes nothing.

Usage:
  apply_headers.py [--check] [PATH ...]
"""

import argparse
import sys
from pathlib import Path

import check_headers


def header_for(path: Path, eol: str = '') -> list[str]:
    """The two header lines in the comment style the path takes."""
    prefix = '#' if check_headers.style_for(path) == 'hash' else '//'
    return [
        f'{prefix} {check_headers.SPDX_MARKER}{eol}',
        f'{prefix} {check_headers.HOLDER}{eol}',
    ]


def read_lines(path: Path) -> tuple[list[str], str]:
    """The file's lines with their ending style, kept byte for byte.

    The read disables newline translation, so a CRLF file keeps its
    carriage returns and the applier only adds header lines, never
    renormalizing the rest of the file.
    """
    with open(path, encoding='utf-8', newline='') as handle:
        text = handle.read()
    eol = '\r' if '\r\n' in text else ''
    return text.split('\n'), eol


def write_lines(path: Path, lines: list[str]) -> None:
    """Write lines back without translating their endings."""
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write('\n'.join(lines))


def header_span(lines: list[str], style: str) -> tuple[int, int]:
    """The start and end of the leading comment run, past shebang lines."""
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if stripped.startswith('#!') or 'coding=' in stripped or 'coding:' in stripped:
            i += 1
        elif not stripped:
            i += 1
        else:
            break
    if i < len(lines) and style == 'hash' and lines[i].lstrip().startswith('#'):
        end = i
        while end < len(lines) and lines[end].lstrip().startswith('#'):
            end += 1
        return i, end
    if i < len(lines) and lines[i].lstrip().startswith('//'):
        end = i
        while end < len(lines) and lines[end].lstrip().startswith('//'):
            end += 1
        return i, end
    if i < len(lines) and lines[i].lstrip().startswith('/*'):
        end = i
        while end < len(lines):
            end += 1
            if '*/' in lines[end - 1]:
                break
        return i, end
    return i, i


def needs_change(path: Path, insert: bool = True) -> bool:
    """True when the file still wants a header applied."""
    try:
        lines, _ = read_lines(path)
    except (OSError, UnicodeDecodeError):
        return False
    start, end = header_span(lines, check_headers.style_for(path))
    run = '\n'.join(lines[start:end])
    if check_headers.GENERATED_MARKER in run:
        return False
    if check_headers.OLD_MIT_MARKER in run:
        return True
    if check_headers.SPDX_MARKER in run and check_headers.HOLDER in run:
        return False
    return insert


def apply_to_file(path: Path, insert: bool = True) -> bool:
    """Bring one file to the SPDX header; True when the file changed."""
    if not needs_change(path, insert):
        return False
    try:
        lines, eol = read_lines(path)
    except (OSError, UnicodeDecodeError):
        return False
    start, end = header_span(lines, check_headers.style_for(path))
    run = '\n'.join(lines[start:end])
    if check_headers.OLD_MIT_MARKER in run:
        lines[start:end] = header_for(path, eol)
        end = start + 2
        if end < len(lines) - 1 and lines[end].strip():
            lines[end:end] = [eol]
    else:
        lines[start:start] = header_for(path, eol) + [eol]
    write_lines(path, lines)
    return True


def iter_targets(root: Path) -> list[Path]:
    """Every file the gate holds to the header, in the gate's own scope."""
    return sorted(
        p
        for p in check_headers.candidate_files(root)
        if p.is_file() and check_headers.in_scope(p, root)
    )


def targets_for(paths: list[str], root: Path) -> list[tuple[Path, bool]]:
    """Named paths with their insertion flag; directories walk the scope."""
    targets = []
    for raw in paths:
        path = Path(raw)
        if path.is_dir():
            targets.extend((p, True) for p in iter_targets(path))
        else:
            targets.append((path, False))
    if not targets:
        targets = [(p, True) for p in iter_targets(root)]
    return targets


def is_exempt(path: Path, root: Path) -> bool:
    """Vendored sources and licence files never take a header."""
    try:
        rel = path.resolve().relative_to(root).as_posix()
    except ValueError:
        return False
    prefixes = tuple(prefix for prefix, _ in check_headers.EXEMPT_PREFIXES)
    if rel.startswith(prefixes):
        return True
    return path.name in ('LICENSE', 'LICENSE.txt')


def main(argv: list[str] | None = None) -> int:
    """Apply the header to the tree or the named paths."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='list files that would change')
    parser.add_argument('paths', nargs='*', help='files or directories (default: the repository)')
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parent.parent
    changed = []
    for path, insert in targets_for(args.paths, root):
        if is_exempt(path, root):
            continue
        if args.check:
            if needs_change(path, insert):
                changed.append(path)
        elif apply_to_file(path, insert=insert):
            changed.append(path)
    for path in changed:
        print(path)
    if args.check:
        return 1 if changed else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
