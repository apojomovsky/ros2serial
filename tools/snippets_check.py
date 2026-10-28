#!/usr/bin/env python3
"""Compile every code snippet fenced in README.md and docs/.

A fenced block tagged c, cpp or python is a snippet and must compile with
the toolchains already pinned in ci/versions.env: gcc and g++ from the dev
image, the standard library for the rest. A plain fence is prose or a
diagram and is ignored. A fence tagged sh, console, text, json or yaml
shows usage or data, so it is skipped and listed as skipped by tag.
Any other tag is an error, so a new language either extends this tool
or loses its tag.

A snippet that cannot compile yet carries the ticket that will make it do so
alone on the line above the fence:

    <!-- snippet: skip until #94 -->

A snippet that never compiles, like a template with placeholder names, keeps
its marker with the reason after a colon:

    <!-- snippet: skip until #75: template placeholders, never literal C -->

A marker without a ticket number is an error, and so is a marker above no
fence. A line that only mentions a marker in passing is not one. The run
prints every skipped snippet, with its ticket and reason when it has them, so none is forgotten. C and C++ snippets compile with -I core/include, -I cpp/include and the staged generated-header fixtures (the golden generator output as r2s_generated.h, the bundle sketch header as ros2serial.h), so snippets using today's real headers compile instead of being skipped.
"""

import argparse
import py_compile
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKER = re.compile(r"^ {0,3}<!--[ \t]*snippet:[ \t]*skip[ \t]+until[ \t]+#([0-9]+)(?::[ \t]*(\S.*?))?[ \t]*-->\s*$")
MARKER_LIKE = re.compile(r"^ {0,3}<!--[ \t]*snippet:.*-->\s*$")
MARKER_EMPTY = re.compile(r"^ {0,3}<!--[ \t]*snippet:[ \t]*skip[ \t]+until[ \t]+#[0-9]+[ \t]*:[ \t]*-->\s*$")
GENERATED_FIXTURES = (
    ("gen/tests/golden/topics/posix.h", "r2s_generated.h"),
    ("gen/tests/golden/bundle/ros2serial.h", "ros2serial.h"),
    ("gen/tests/golden/bundle/r2s_bundled.h", "r2s_bundled.h"),
)
# A fence opens at column 0 to 3 of its container: deeper is an indented code
# block, literal text. The closer sits no deeper than three past the container;
# anything deeper is body text. A tagged fence the scan never opens, inside a
# body or a quote, is reported instead of silently passing. Indentation is
# relative to list items, which the scan tracks; a dedented line pops back
# out. A marker past column 3 of its container gets its own diagnostic.
FENCE = re.compile(r"^ *```")
MARKER_ANY = re.compile(r"^\s*<!--[ \t]*snippet:.*-->\s*$")
SWEEP = re.compile(r"^\s*(?:>\s*)*```([A-Za-z0-9+#-]+)")

LANGUAGES = ("c", "cpp", "python")
EXTENSION = {"c": ".c", "cpp": ".cpp", "python": ".py"}
SKIPPED_TAGS = ("console", "json", "sh", "text", "yaml")


@dataclass
class Snippet:
    path: str
    line: int
    language: str
    code: str
    ticket: int | None
    reason: str | None = None
    skipped_tag: str | None = None


def _ticket(marker_line: str) -> int:
    # Only called on lines MARKER already matched, so group(1) exists.
    return int(MARKER.match(marker_line).group(1))


def _reason(marker_line: str) -> str | None:
    # Only called on lines MARKER already matched, so group(2) is the reason or None.
    matched = MARKER.match(marker_line)
    return matched.group(2).strip() if matched.group(2) else None


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


LIST_ITEM = re.compile(r"( {0,3})([*+-]|\d{1,9}[.)])([ \t]*)")


def _list_content(line: str, base: int) -> int | None:
    # Content indent of the list item `line` starts, if it starts one in the
    # container whose content starts at `base`. A missing separator means it
    # starts none, as do five or more spaces after the marker, which count as
    # one. Both rules follow CommonMark.
    rest = line[base:]
    match = LIST_ITEM.match(rest)
    if match is None:
        return None
    lead, marker, gaps = match.groups()
    if not gaps and match.end() < len(rest):
        return None
    gap = len(gaps) if 1 <= len(gaps) <= 4 and rest[match.end() :].strip() else 1
    return base + len(lead) + len(marker) + gap


def _marker_kind(line: str, base: int) -> str:
    # What the marker-shaped comment on `line` is worth at container `base`:
    # usable, ticketless, empty of its reason, or literal text past column 3.
    rel = line[base:]
    if MARKER.match(rel):
        return "ok"
    if MARKER_EMPTY.match(rel):
        return "empty"
    if MARKER_LIKE.match(rel):
        return "ticketless"
    return "deep"


def _marker_problem(kind: str) -> str:
    if kind == "ok":
        return "snippet marker above no fence"
    if kind == "ticketless":
        return "snippet marker without a ticket number"
    if kind == "empty":
        return "snippet marker reason is empty after the colon"
    return "snippet marker starts past column 3, so it is literal text"


def extract(path: Path, rel: str) -> tuple[list[Snippet], list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    snippets: list[Snippet] = []
    problems: list[str] = []
    markers: dict[int, str] = {}
    opened: set[int] = set()
    used: set[int] = set()
    i = 0
    stack: list[int] = []
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        raw = _indent_of(line)
        while stack and raw < stack[-1]:
            stack.pop()
        base = stack[-1] if stack else 0
        if MARKER_ANY.match(line):
            markers[i + 1] = _marker_kind(line, base)
            i += 1
            continue
        fence = FENCE.match(line)
        if fence and 0 <= raw - base <= 3:
            words = line[fence.end() :].split()
            open_line = i + 1
            if words and (words[0] in LANGUAGES or words[0] in SKIPPED_TAGS):
                opened.add(open_line)
            indent = raw
            i += 1
            body: list[str] = []
            while i < len(lines):
                closer = FENCE.match(lines[i])
                if closer and _indent_of(lines[i]) - base <= 3:
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
            reason = None
            j = open_line - 2
            while j >= 0 and not lines[j].strip():
                j -= 1
            if j >= 0 and MARKER_ANY.match(lines[j]):
                probe = list(stack)
                while probe and _indent_of(lines[j]) < probe[-1]:
                    probe.pop()
                kind = _marker_kind(lines[j], probe[-1] if probe else 0)
                if kind == "ok":
                    marker = lines[j][probe[-1] if probe else 0 :]
                    ticket = _ticket(marker)
                    reason = _reason(marker)
                else:
                    problems.append(f"{rel}:{j + 1}: {_marker_problem(kind)}")
                used.add(j + 1)
            if len(words) > 1:
                problems.append(f"{rel}:{open_line}: unexpected info string after {words[0]!r}")
                continue
            language = words[0]
            if language in SKIPPED_TAGS:
                snippets.append(Snippet(rel, open_line, language, "", ticket, reason=reason, skipped_tag=language))
                continue
            if language not in LANGUAGES:
                problems.append(f"{rel}:{open_line}: unknown snippet language {language!r}")
                continue
            code = "\n".join(_dedent(body_line, indent) for body_line in body) + "\n"
            snippets.append(Snippet(rel, open_line, language, code, ticket, reason=reason))
            continue
        content = _list_content(line, base)
        if content is not None:
            if stack and stack[-1] == content:
                stack.pop()
            stack.append(content)
        i += 1
        continue
    for lineno, kind in markers.items():
        if lineno in used:
            continue
        problems.append(f"{rel}:{lineno}: {_marker_problem(kind)}")
    for number, text in enumerate(lines, start=1):
        tag = SWEEP.match(text)
        if tag and tag.group(1) in LANGUAGES + SKIPPED_TAGS and number not in opened:
            problems.append(f"{rel}:{number}: tagged fence never opened a snippet")
    return snippets, problems


def _stage_fixtures(root: Path, workdir: Path) -> None:
    # Generated-header fixtures the snippets include: the golden generator
    # output under the name firmware uses, and the bundle sketch header.
    # Missing files stay out, so a tree without them behaves as before and a
    # snippet needing one fails with the compiler's file-not-found error.
    for src, name in GENERATED_FIXTURES:
        fixture = root / src
        if fixture.is_file():
            shutil.copyfile(fixture, workdir / name)


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
    # The staged generated-header fixtures live in the workdir, so a snippet
    # including r2s_generated.h or ros2serial.h finds them there. The core's
    # headers and the C++ layer's own headers ride on -I, so the runtime
    # API and the message headers resolve too.
    cmd += ["-I", str(workdir)]
    for include in (root / "core" / "include", root / "cpp" / "include"):
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
    try:
        py_compile.compile(str(src), doraise=True)
    except py_compile.PyCompileError as exc:
        return f"{snippet.path}:{snippet.line}: python snippet does not compile: {exc.msg}"
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
    compiled = skipped = by_tag = 0
    with tempfile.TemporaryDirectory() as work:
        workdir = Path(work)
        _stage_fixtures(root, workdir)
        for path in files:
            rel = str(path.relative_to(root))
            snippets, found = extract(path, rel)
            problems += found
            for snippet in snippets:
                if snippet.skipped_tag is not None:
                    extra = ""
                    if snippet.ticket is not None:
                        extra = f" (until #{snippet.ticket}"
                        extra += f": {snippet.reason}" if snippet.reason else ""
                        extra += ")"
                    print(f"skip {rel}:{snippet.line} ({snippet.skipped_tag}): skipped by tag, not compiled{extra}")
                    by_tag += 1
                    continue
                if snippet.ticket is not None:
                    why = f": {snippet.reason}" if snippet.reason else ""
                    print(f"skip {rel}:{snippet.line} ({snippet.language}) until #{snippet.ticket}{why}")
                    skipped += 1
                    continue
                failed = compile_snippet(snippet, root, workdir)
                if failed is None:
                    compiled += 1
                else:
                    problems.append(failed)
    print(f"docs snippets: {compiled} compiled, {skipped} skipped, {by_tag} skipped by tag")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compile every code snippet fenced in README.md and docs/.",
        epilog="Fences tagged c, cpp or python must compile. Fences tagged "
        + ", ".join(SKIPPED_TAGS)
        + " are usage or data and are skipped. Any other tag is an error. "
        "A snippet that cannot compile yet carries its ticket alone on the "
        "line above the fence: <!-- snippet: skip until #NN -->. A snippet "
        "that never compiles keeps its ticket with the reason after a colon.",
    )
    parser.parse_args(argv)
    problems = check_tree(ROOT)
    for problem in problems:
        print(problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
