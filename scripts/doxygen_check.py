#!/usr/bin/env python3
"""Check the Doxygen form of doc comments on public C and C++ headers.

The C core follows the rcl header style and the C++ bridge the rclcpp one
(AGENTS.md): every public function carries a ``///`` one-line summary plus
a ``/** */`` body with ``\\param``, ``\\return`` and ``\\pre`` as each
applies; every public macro, type, alias and constant carries at least a
``///`` summary. Plain ``/* */`` prose above a declaration is not a doc
comment. This check is the gate behind the Doxygen-form tickets: it fails
on a header that still uses the old prose contracts.

Usage:
  doxygen_check.py [PATH ...]   check the headers under each path
                                (default: core/include and bridge/r2s_bridge/include)
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
ACCESS_RE = re.compile(r'^(public|protected|private)\s*:')
TYPE_RE = re.compile(r'\s*(class|struct|enum\s+class|enum\s+struct|enum|union)\s+([A-Za-z_]\w*)')
USING_RE = re.compile(r'\s*using\s+([A-Za-z_]\w*)\s*=')
OPERATOR_RE = re.compile(r'operator\s*([^\s(]+)')
VAR_RE = re.compile(r'([A-Za-z_]\w*)\s*(=[^;\n]*)?;\s*$')
NAMESPACE_RE = re.compile(r'\s*namespace\s*([A-Za-z_]\w*)?\s*(\{)?\s*$')
NON_FUNC_NAMES = ('if', 'while', 'for', 'switch', 'return', 'static_assert')


@dataclass
class Decl:
    """One public declaration needing a doc comment."""

    kind: str  # 'func', 'macro', 'typedef', 'type', 'alias', 'const' or 'var'
    name: str
    line: int  # 1-based start line
    params: list[str] = field(default_factory=list)
    returns_void: bool = True
    no_tags: bool = False  # deleted, defaulted: summary only


@dataclass
class Scope:
    """One brace level: what declarations inside it are public."""

    kind: str  # 'root', 'namespace', 'anon', 'class', 'struct', 'enum' or 'union'
    name: str | None = None
    access: str = 'public'
    depth: int = 0


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
    part = _split_top_commas(part)[0].split('=')[0]
    found = IDENT_RE.findall(part)
    if not found or found[-1] in ('const', 'volatile'):
        return None
    return found[-1]


def _parse_func(buf: str, line: int, class_name: str | None = None) -> Decl | None:
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
    head = re.sub(r'template\s*<.*?>\s*', '', head)
    op_match = OPERATOR_RE.search(head)
    if op_match:
        name = 'operator' + op_match.group(1)
    else:
        name_match = list(IDENT_RE.finditer(head))
        if not name_match:
            return None
        name = name_match[-1].group(0)
    if name in NON_FUNC_NAMES:
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
    if not (len(raw_params) == 1 and raw_params[0].strip() in ('void', '')):
        for part in raw_params:
            pname = _param_name(part)
            if pname is not None:
                params.append(pname)
    ret = head[: len(head)].strip()
    ret = re.sub(r'\b(static|inline|extern|virtual|explicit|constexpr|friend)\b', '', ret).strip()
    if not op_match:
        ret = ret[: len(ret) - len(name)].strip() if ret.endswith(name) else ret
    returns_void = ret in ('void', '')
    if class_name is not None and (name == class_name or name == 'operator='):
        returns_void = True
    if re.match(r'\s*~', head):
        returns_void = True
        params = []
    tail = rest[close_at + 1 :] if close_at >= 0 else ''
    if re.search(r'=\s*(delete|default)\s*;', tail):
        return Decl(kind='func', name=name, line=line, no_tags=True)
    return Decl(kind='func', name=name, line=line, params=params, returns_void=returns_void)


def _parse_typedef(buf: str, line: int) -> Decl | None:
    match = re.search(r'([A-Za-z_]\w*)\s*;\s*$', buf)
    if not match:
        return None
    return Decl(kind='typedef', name=match.group(1), line=line)


def _default_access(kind: str) -> str:
    """The member access a new scope of this kind starts with."""
    return 'private' if kind == 'class' else 'public'


def _is_public(stack: list[Scope]) -> bool:
    """Whether the innermost scope exposes declarations as public API."""
    top = stack[-1]
    if top.kind == 'anon':
        return False
    return top.access == 'public'


def _enclosing_class(stack: list[Scope]) -> str | None:
    """The name of the innermost class or struct scope, if any."""
    for scope in reversed(stack):
        if scope.kind in ('class', 'struct'):
            return scope.name
    return None


def _record_macro(raw_stripped: str, idx: int, guards: set[str], decls: list[Decl]) -> None:
    """Record one preprocessor definition, unless it is an include guard."""
    define = DEFINE_RE.match(raw_stripped)
    if define is None:
        return
    name = define.group(1)
    if name in guards and GUARD_RE.fullmatch(name):
        return
    params: list[str] = []
    if define.group(2) is not None:
        for part in define.group(3).split(','):
            pname = _param_name(part)
            if pname is not None:
                params.append(pname)
    decls.append(Decl(kind='macro', name=name, line=idx + 1, params=params))


FUNC_SIG_RE = re.compile(
    r'\)\s*(?:const\s+)?(?:noexcept(?:\([^)]*\))?\s+)?(?:override\s+)?(?:final\s+)?'
    r'(?:=\s*(?:0|delete|default)\s*)?$'
)


def _is_function(buf: str) -> bool:
    """Whether a complete statement declares or defines a function.

    A function declarator ends in ``)`` plus qualifiers; a data member
    whose type holds parentheses (``std::function<...>``) ends in its name.
    """
    text = buf.strip()
    if text.endswith(';'):
        text = text[:-1]
    elif '{' in text:
        text = text.split('{')[0]
    else:
        return False
    return bool(FUNC_SIG_RE.search(text))


def _record_statement(buf: str, start: int, stack: list[Scope], decls: list[Decl]) -> None:
    """Classify one complete statement and record it when it is public."""
    if re.match(r'\s*typedef\b', buf):
        parsed = _parse_typedef(buf, start)
        if parsed is not None:
            decls.append(parsed)
        return
    if USING_RE.match(buf):
        if _is_public(stack):
            decls.append(Decl(kind='alias', name=USING_RE.match(buf).group(1), line=start))
        return
    if _is_function(buf):
        if not _is_public(stack):
            return
        parsed = _parse_func(buf, start, _enclosing_class(stack))
        if parsed is not None:
            decls.append(parsed)
        return
    if 'constexpr' in buf:
        if not _is_public(stack):
            return
        match = VAR_RE.search(buf)
        if match is not None:
            decls.append(Decl(kind='const', name=match.group(1), line=start))
        return
    if stack[-1].kind in ('class', 'struct', 'enum', 'union'):
        return
    if not _is_public(stack):
        return
    match = VAR_RE.search(buf)
    if match is not None and match.group(1) not in ('return',):
        decls.append(Decl(kind='var', name=match.group(1), line=start))


def _collect(text: str) -> list[Decl]:
    """Every documented-or-not public declaration in one header."""
    lines = text.splitlines()
    codes = _code_lines(text)
    decls: list[Decl] = []
    guards: set[str] = set()
    stack: list[Scope] = [Scope(kind='root', depth=0)]
    depth = 0
    buf = ''
    start = 0
    pending: Scope | None = None
    for idx, code in enumerate(codes):
        stripped = code.strip()
        raw_stripped = lines[idx].lstrip()
        if raw_stripped.startswith('#'):
            if not buf and pending is None:
                guard = IFNDEF_RE.match(raw_stripped)
                if guard:
                    guards.add(guard.group(1))
                else:
                    _record_macro(raw_stripped, idx, guards, decls)
        elif pending is not None:
            if '{' not in code and ';' in code:
                pending = None
        else:
            access = ACCESS_RE.match(stripped)
            if access is not None and stack[-1].kind in ('class', 'struct'):
                stack[-1].access = access.group(1)
            elif not stripped or stripped.startswith('extern'):
                continue
            elif stripped in ('{', '}') and not buf:
                continue
            else:
                type_match = TYPE_RE.match(code)
                ns_match = None if type_match else NAMESPACE_RE.match(code)
                if type_match is not None:
                    word, name = type_match.group(1), type_match.group(2)
                    kind = 'enum' if word.startswith('enum') else word
                    after = code[type_match.end() :]
                    if _is_public(stack):
                        decls.append(Decl(kind='type', name=name, line=idx + 1))
                    if '{' in after:
                        stack.append(
                            Scope(
                                kind=kind,
                                name=name,
                                access=_default_access(kind),
                                depth=depth + 1,
                            )
                        )
                    elif ';' not in after:
                        pending = Scope(kind=kind, name=name, access=_default_access(kind))
                    buf = ''
                elif ns_match is not None and not buf:
                    pending = Scope(
                        kind='anon' if ns_match.group(1) is None else 'namespace',
                        name=ns_match.group(1),
                    )
                    if ns_match.group(2) is not None:
                        stack.append(pending)
                        pending.depth = depth + 1
                        pending = None
                    buf = ''
                else:
                    if not buf:
                        start = idx + 1
                    buf += code + '\n'
        opens = code.count('{')
        closes = code.count('}')
        depth += opens - closes
        if pending is not None and opens > 0:
            pending.depth = depth
            stack.append(pending)
            pending = None
            buf = ''
        while len(stack) > 1 and stack[-1].depth > depth:
            stack.pop()
        if pending is None and buf and depth == stack[-1].depth:
            if re.search(r';\s*$', buf):
                _record_statement(buf, start, stack, decls)
                buf = ''
            elif re.search(r'\}\s*$', buf):
                _record_statement(buf, start, stack, decls)
                buf = ''
    return decls


def _skippable(line: str) -> bool:
    """Whether a line between a declaration and its doc comment is neutral."""
    stripped = line.strip()
    return stripped == '' or line.lstrip().startswith('#') or line.lstrip().startswith('// NOLINT')


def _doc_above(lines: list[str], start: int) -> tuple[str, bool, str | None]:
    """The doc text above 1-based line start: (doc, has body, problem).

    The rcl form is a ``///`` summary above a ``/** */`` body, the body
    directly above the declaration. Linter directives such as ``NOLINT``
    between the comment and the declaration are neutral.
    """
    i = start - 2
    while i >= 0 and _skippable(lines[i]):
        i -= 1
    if i < 0:
        return '', False, 'undocumented public symbol'
    body = ''
    if '/*' in lines[i] or '*/' in lines[i] or lines[i].lstrip().startswith('*'):
        end = i
        while i >= 0 and '/*' not in lines[i]:
            i -= 1
        if i < 0 or '/**' not in lines[i]:
            return '', False, 'comment is not Doxygen form (/// or /** */)'
        body = '\n'.join(lines[i : end + 1])
        i -= 1
        while i >= 0 and _skippable(lines[i]):
            i -= 1
        if i < 0 or not lines[i].lstrip().startswith('///'):
            return '', False, 'missing /// one-line summary'
    if i >= 0 and lines[i].lstrip().startswith('///'):
        built = []
        while i >= 0 and lines[i].lstrip().startswith('///'):
            built.append(lines[i].lstrip()[3:].strip())
            i -= 1
        summary = ' '.join(reversed(built)).strip()
        if not summary:
            return '', False, 'missing /// one-line summary'
        return (summary + '\n' + body).strip(), bool(body), None
    if body:
        return '', False, 'missing /// one-line summary'
    if lines[i].lstrip().startswith('/*'):
        return '', False, 'comment is not Doxygen form (/// or /** */)'
    return '', False, 'undocumented public symbol'


def _needs_tags(decl: Decl) -> bool:
    """Whether the declaration carries parameters or a return value."""
    return decl.kind == 'func' and not decl.no_tags and bool(decl.params or not decl.returns_void)


def check_text(label: str, text: str) -> list[str]:
    """Every Doxygen-form problem in one header's text."""
    problems = []
    lines = text.splitlines()
    for decl in _collect(text):
        doc, has_body, problem = _doc_above(lines, decl.line)
        if problem is not None:
            problems.append(f'{label}:{decl.line}: {decl.name}: {problem}')
            continue
        if decl.kind == 'func' or (decl.kind == 'macro' and decl.params):
            if _needs_tags(decl) and not has_body:
                problems.append(f'{label}:{decl.line}: {decl.name}: parameters need a /** */ body')
                continue
            tagged = set(PARAM_RE.findall(doc))
            for pname in decl.params:
                if pname not in tagged:
                    problems.append(
                        f'{label}:{decl.line}: {decl.name}: missing \\param for {pname!r}'
                    )
            if (
                decl.kind == 'func'
                and not decl.no_tags
                and not decl.returns_void
                and not RETURN_RE.search(doc)
            ):
                problems.append(f'{label}:{decl.line}: {decl.name}: missing \\return')
    return problems


def check_headers(paths: list[Path]) -> list[str]:
    """Every Doxygen-form problem in the headers under each path."""
    problems = []
    headers: list[Path] = []
    for path in paths:
        if path.is_dir():
            headers.extend(sorted(path.rglob('*.h')))
            headers.extend(sorted(path.rglob('*.hpp')))
        elif path.suffix in ('.h', '.hpp'):
            headers.append(path)
    for header in headers:
        problems.extend(check_text(str(header), header.read_text(encoding='utf-8')))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='*', default=['core/include', 'bridge/r2s_bridge/include'])
    args = parser.parse_args()
    problems = check_headers([Path(p) for p in args.paths])
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
