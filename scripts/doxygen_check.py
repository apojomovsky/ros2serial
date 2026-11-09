#!/usr/bin/env python3
"""Check the Doxygen form of doc comments on public C headers.

The C core follows the rcl header style (AGENTS.md): every public function
carries a ``///`` one-line summary plus a ``/** */`` body with ``\\param``,
``\\return`` and ``\\pre`` as each applies; every public macro and type
carries at least a ``///`` summary. Plain ``/* */`` prose above a
declaration is not a doc comment. This check is the gate behind the
Doxygen-form ticket: it fails on a header that still uses the old prose
contracts.

Usage:
  doxygen_check.py [PATH ...]   check the headers under each path (default: core/include)
"""

import argparse
import re
import sys
from dataclasses import dataclass
from dataclasses import field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PARAM_RE = re.compile(r'\\param\[(?:in|out|in,out|inout)\]\s+([A-Za-z_]\w*)')
RETURN_RE = re.compile(r'\\return\b')
IDENT_RE = re.compile(r'[A-Za-z_]\w*')
DEFINE_RE = re.compile(r'#\s*define\s+([A-Za-z_]\w*)(\(([^)]*)\))?')
IFNDEF_RE = re.compile(r'#\s*ifndef\s+([A-Za-z_]\w*)')
GUARD_RE = re.compile(r'.*_H(PP)?_$|.*_H(PP)?$')


@dataclass
class Decl:
    """One public declaration needing a doc comment."""

    kind: str  # 'func', 'macro' or 'typedef'
    name: str
    line: int  # 1-based start line
    params: list[str] = field(default_factory=list)
    returns_void: bool = True


def _code_lines(text: str) -> list[str]:
    """The file with comments blanked, keeping line numbers."""
    codes = []
    in_block = False
    for raw in text.splitlines():
        out = []
        i = 0
        while i < len(raw):
            if in_block:
                end = raw.find('*/', i)
                if end < 0:
                    break
                in_block = False
                i = end + 2
            elif raw.startswith('//', i):
                break
            elif raw.startswith('/*', i):
                in_block = True
                i += 2
            elif raw[i] == '"':
                end = raw.find('"', i + 1)
                i = len(raw) if end < 0 else end + 1
            elif raw[i] == "'":
                end = raw.find("'", i + 1)
                i = len(raw) if end < 0 else end + 1
            else:
                out.append(raw[i])
                i += 1
        codes.append(''.join(out))
    return codes


def _split_top_commas(text: str) -> list[str]:
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return parts


def _param_name(part: str) -> str | None:
    part = part.strip()
    if not part or part == '...':
        return None
    found = IDENT_RE.findall(part)
    return found[-1] if found else None


def _parse_func(buf: str, line: int) -> Decl | None:
    depth = 0
    open_at = -1
    for i, ch in enumerate(buf):
        if ch == '(':
            if depth == 0 and open_at < 0:
                open_at = i
            depth += 1
        elif ch == ')':
            depth -= 1
    if open_at < 0:
        return None
    head = buf[:open_at]
    name_match = list(IDENT_RE.finditer(head))
    if not name_match:
        return None
    name = name_match[-1].group(0)
    if name in ('if', 'while', 'for', 'switch', 'return'):
        return None
    rest = buf[open_at + 1 :]
    depth = 1
    close_at = -1
    for i, ch in enumerate(rest):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                close_at = i
                break
    if close_at < 0:
        return None
    raw_params = _split_top_commas(rest[:close_at])
    params = []
    if not (len(raw_params) == 1 and raw_params[0].strip() == 'void'):
        for part in raw_params:
            pname = _param_name(part)
            if pname is not None:
                params.append(pname)
    ret = head[: name_match[-1].start()].strip()
    ret = re.sub(r'\b(static|inline|extern)\b', '', ret).strip()
    returns_void = ret == 'void'
    return Decl(kind='func', name=name, line=line, params=params, returns_void=returns_void)


def _parse_typedef(buf: str, line: int) -> Decl | None:
    match = re.search(r'([A-Za-z_]\w*)\s*;\s*$', buf)
    if not match:
        return None
    return Decl(kind='typedef', name=match.group(1), line=line)


def _collect(text: str) -> list[Decl]:
    """Every documented-or-not public declaration in one header."""
    lines = text.splitlines()
    codes = _code_lines(text)
    decls: list[Decl] = []
    guards: set[str] = set()
    buf = ''
    start = 0
    depth = 0
    for idx, code in enumerate(codes):
        stripped = code.strip()
        raw_stripped = lines[idx].lstrip()
        if raw_stripped.startswith('#'):
            if not buf:
                guard = IFNDEF_RE.match(raw_stripped)
                if guard:
                    guards.add(guard.group(1))
                define = DEFINE_RE.match(raw_stripped)
                if define:
                    name = define.group(1)
                    if name not in guards or not GUARD_RE.fullmatch(name):
                        params: list[str] = []
                        if define.group(2) is not None:
                            for part in define.group(3).split(','):
                                pname = _param_name(part)
                                if pname is not None:
                                    params.append(pname)
                        decls.append(Decl(kind='macro', name=name, line=idx + 1, params=params))
            continue
        if not stripped or stripped.startswith('extern'):
            continue
        if stripped in ('{', '}') and not buf:
            continue
        if not buf:
            start = idx + 1
        buf += code + '\n'
        for ch in code:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
        if depth > 0:
            continue
        if re.search(r';\s*$', buf):
            if re.match(r'\s*typedef\b', buf):
                parsed = _parse_typedef(buf, start)
                if parsed is not None:
                    decls.append(parsed)
            elif '(' in buf:
                parsed = _parse_func(buf, start)
                if parsed is not None:
                    decls.append(parsed)
            buf = ''
        elif re.search(r'\}\s*$', buf):
            if '(' in buf.split('{')[0]:
                parsed = _parse_func(buf, start)
                if parsed is not None:
                    decls.append(parsed)
            buf = ''
            depth = 0
    return decls


def _doc_above(lines: list[str], start: int) -> tuple[str, str | None]:
    """The doc text above 1-based line start: (doc, problem).

    The rcl form is a ``///`` summary above a ``/** */`` body, the body
    directly above the declaration.
    """
    i = start - 2
    while i >= 0 and (lines[i].strip() == '' or lines[i].lstrip().startswith('#')):
        i -= 1
    if i < 0:
        return '', 'undocumented public symbol'
    body = ''
    if '/*' in lines[i] or '*/' in lines[i] or lines[i].lstrip().startswith('*'):
        end = i
        while i >= 0 and '/*' not in lines[i]:
            i -= 1
        if i < 0 or '/**' not in lines[i]:
            return '', 'comment is not Doxygen form (/// or /** */)'
        body = '\n'.join(lines[i : end + 1])
        i -= 1
        while i >= 0 and lines[i].strip() == '':
            i -= 1
        if i < 0 or not lines[i].lstrip().startswith('///'):
            return '', 'missing /// one-line summary'
    if i >= 0 and lines[i].lstrip().startswith('///'):
        built = []
        while i >= 0 and lines[i].lstrip().startswith('///'):
            built.append(lines[i].lstrip()[3:].strip())
            i -= 1
        summary = ' '.join(reversed(built)).strip()
        if not summary:
            return '', 'missing /// one-line summary'
        return (summary + '\n' + body).strip(), None
    if body:
        return '', 'missing /// one-line summary'
    if lines[i].lstrip().startswith('/*'):
        return '', 'comment is not Doxygen form (/// or /** */)'
    return '', 'undocumented public symbol'


def check_text(label: str, text: str) -> list[str]:
    """Every Doxygen-form problem in one header's text."""
    problems = []
    lines = text.splitlines()
    for decl in _collect(text):
        doc, problem = _doc_above(lines, decl.line)
        if problem is not None:
            problems.append(f'{label}:{decl.line}: {decl.name}: {problem}')
            continue
        if decl.kind == 'func' or (decl.kind == 'macro' and decl.params):
            tagged = set(PARAM_RE.findall(doc))
            for pname in decl.params:
                if pname not in tagged:
                    problems.append(
                        f'{label}:{decl.line}: {decl.name}: missing \\param for {pname!r}'
                    )
            if decl.kind == 'func' and not decl.returns_void and not RETURN_RE.search(doc):
                problems.append(f'{label}:{decl.line}: {decl.name}: missing \\return')
    return problems


def check_headers(paths: list[Path]) -> list[str]:
    """Every Doxygen-form problem in the headers under each path."""
    problems = []
    headers: list[Path] = []
    for path in paths:
        if path.is_dir():
            headers.extend(sorted(path.rglob('*.h')))
        elif path.suffix == '.h':
            headers.append(path)
    for header in headers:
        problems.extend(check_text(str(header), header.read_text(encoding='utf-8')))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='*', default=['core/include'])
    args = parser.parse_args()
    problems = check_headers([Path(p) for p in args.paths])
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
