#!/usr/bin/env python3
"""Compile every code snippet fenced in README.md and docs/.

A fenced block with a language tag (c, cpp, python, yaml) is a snippet and
must compile with the toolchains already pinned in ci/versions.env: gcc and
g++ from the dev image, the standard library for the rest. A plain fence is
prose or a diagram and is ignored. An unknown tag is an error, so a new
language either extends this tool or loses its tag.

A snippet that cannot compile yet carries the ticket that will make it do so
alone on the line above the fence:

    <!-- snippet: skip until #94 -->

A marker without a ticket number is an error, and so is a marker above no
fence. A line that only mentions a marker in passing is not one. The run
prints every skipped snippet with its ticket, so none is forgotten. C and C++
snippets compile with -I core/include, so snippets using today's real
headers compile instead of being skipped.
"""

import py_compile
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
MARKER = re.compile(r"^ {0,3}<!--[ \t]*snippet:[ \t]*skip[ \t]+until[ \t]+#([0-9]+)[ \t]*-->\s*$")
MARKER_LIKE = re.compile(r"^ {0,3}<!--[ \t]*snippet:.*-->\s*$")
# A fence opens at column 0 to 3 only: deeper is an indented code block,
# literal text, so a literal fence can never swallow a real snippet. The
# closer must sit at the opener's own indent; anything else is body text. A
# marker deeper than three spaces is literal text too, so it gets its own
# diagnostic instead of silently doing nothing.
FENCE = re.compile(r"^ {0,3}```")
MARKER_ANY = re.compile(r"^\s*<!--[ \t]*snippet:.*-->\s*$")

LANGUAGES = ("c", "cpp", "python", "yaml")
EXTENSION = {"c": ".c", "cpp": ".cpp", "python": ".py", "yaml": ".yaml"}


@dataclass
class Snippet:
    path: str
    line: int
    language: str
    code: str
    ticket: int | None


def _ticket(marker_line: str) -> int:
    # Only called on lines MARKER already matched, so group(1) exists.
    return int(MARKER.match(marker_line).group(1))


def _dedent(line: str, width: int) -> str:
    # CommonMark strips up to the fence's own indentation from each body line.
    taken = 0
    while taken < width and taken < len(line) and line[taken] == " ":
        taken += 1
    return line[taken:]


def _indent_of(line: str) -> int:
    taken = 0
    while taken < len(line) and line[taken] == " ":
        taken += 1
    return taken


def extract(path: Path, rel: str) -> tuple[list[Snippet], list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    snippets: list[Snippet] = []
    problems: list[str] = []
    markers: dict[int, str] = {}
    used: set[int] = set()
    i = 0
    while i < len(lines):
        if MARKER_ANY.match(lines[i]):
            markers[i + 1] = lines[i]
            i += 1
            continue
        fence = FENCE.match(lines[i])
        if not fence:
            i += 1
            continue
        words = lines[i][fence.end() :].split()
        open_line = i + 1
        indent = fence.end() - 3
        i += 1
        body: list[str] = []
        while i < len(lines):
            closer = FENCE.match(lines[i])
            if closer and _indent_of(lines[i]) == indent:
                break
            body.append(lines[i])
            i += 1
        if i == len(lines):
            problems.append(f"{rel}:{open_line}: fence never closes")
            break
        i += 1
        if not words:
            continue
        ticket = None
        j = open_line - 2
        while j >= 0 and not lines[j].strip():
            j -= 1
        if j >= 0 and MARKER.match(lines[j]):
            ticket = _ticket(lines[j])
            used.add(j + 1)
        elif j >= 0 and MARKER_ANY.match(lines[j]):
            if MARKER_LIKE.match(lines[j]):
                problems.append(f"{rel}:{j + 1}: snippet marker without a ticket number")
            else:
                problems.append(f"{rel}:{j + 1}: snippet marker starts past column 3, so it is literal text")
            used.add(j + 1)
        if len(words) > 1:
            problems.append(f"{rel}:{open_line}: unexpected info string after {words[0]!r}")
            continue
        language = words[0]
        if language not in LANGUAGES:
            problems.append(f"{rel}:{open_line}: unknown snippet language {language!r}")
            continue
        code = "\n".join(_dedent(line, fence.end() - 3) for line in body) + "\n"
        snippets.append(Snippet(rel, open_line, language, code, ticket))
    for lineno, text in markers.items():
        if lineno in used:
            continue
        if MARKER.match(text):
            problems.append(f"{rel}:{lineno}: snippet marker above no fence")
        elif MARKER_LIKE.match(text):
            problems.append(f"{rel}:{lineno}: snippet marker without a ticket number")
        else:
            problems.append(f"{rel}:{lineno}: snippet marker starts past column 3, so it is literal text")
    return snippets, problems


def _compile_c(snippet: Snippet, root: Path, workdir: Path) -> str | None:
    compiler = shutil.which("gcc" if snippet.language == "c" else "g++")
    if compiler is None:
        return f"{snippet.path}:{snippet.line}: no compiler for {snippet.language}"
    standard = "c99" if snippet.language == "c" else "c++11"
    # Almost the core's warning set (core/CMakeLists.txt), so a snippet that
    # passes here also passes the firmware build. The prototype flags stay
    # out: snippets define example functions without prior declarations.
    warnings = ["-pedantic", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-Wundef"]
    src = workdir / f"snippet{EXTENSION[snippet.language]}"
    src.write_text(snippet.code)
    cmd = [compiler, f"-std={standard}", *warnings, "-fsyntax-only", str(src)]
    include = root / "core" / "include"
    if include.is_dir():
        cmd += ["-I", str(include)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        return f"{snippet.path}:{snippet.line}: {snippet.language} snippet does not compile:\n{done.stderr.strip()}"
    return None


def compile_snippet(snippet: Snippet, root: Path, workdir: Path) -> str | None:
    """Compile one snippet; None means it passed."""
    if snippet.language in ("c", "cpp"):
        return _compile_c(snippet, root, workdir)
    src = workdir / f"snippet{EXTENSION[snippet.language]}"
    src.write_text(snippet.code)
    if snippet.language == "python":
        try:
            py_compile.compile(str(src), doraise=True)
        except py_compile.PyCompileError as exc:
            return f"{snippet.path}:{snippet.line}: python snippet does not compile: {exc.msg}"
        return None
    try:
        yaml.safe_load(snippet.code)
    except yaml.YAMLError as exc:
        return f"{snippet.path}:{snippet.line}: yaml snippet does not parse: {exc}"
    return None


def check_tree(root: Path) -> list[str]:
    """Check README.md and docs/*.md under root; problems, empty when green."""
    problems: list[str] = []
    files = []
    readme = root / "README.md"
    if not readme.is_file():
        return ["README.md: not found"]
    files.append(readme)
    files += sorted((root / "docs").rglob("*.md")) if (root / "docs").is_dir() else []
    compiled = skipped = 0
    with tempfile.TemporaryDirectory() as work:
        workdir = Path(work)
        for path in files:
            rel = str(path.relative_to(root))
            snippets, found = extract(path, rel)
            problems += found
            for snippet in snippets:
                if snippet.ticket is not None:
                    print(f"skip {rel}:{snippet.line} ({snippet.language}) until #{snippet.ticket}")
                    skipped += 1
                    continue
                failed = compile_snippet(snippet, root, workdir)
                if failed is None:
                    compiled += 1
                else:
                    problems.append(failed)
    print(f"docs snippets: {compiled} compiled, {skipped} skipped")
    return problems


def main() -> int:
    problems = check_tree(ROOT)
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
