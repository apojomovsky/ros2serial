#!/usr/bin/env python3
"""Compile every code snippet fenced in README.md and docs/.

A fenced block with a language tag (c, cpp, python, yaml) is a snippet and
must compile with the toolchains already pinned in ci/versions.env: gcc and
g++ from the dev image, the standard library for the rest. A plain fence is
prose or a diagram and is ignored. An unknown tag is an error, so a new
language either extends this tool or loses its tag.

A snippet that cannot compile yet carries the ticket that will make it do so
on the line above the fence:

    <!-- snippet: skip until #94 -->

A marker without a ticket number is an error, and so is a marker above no
fence. The run prints every skipped snippet with its ticket, so none is
forgotten. C and C++ snippets compile with -I core/include, so snippets
using today's real headers compile instead of being skipped.
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

MARKER = re.compile(r"<!--[ \t]*snippet:[ \t]*skip[ \t]+until[ \t]+#([0-9]+)[ \t]*-->")
MARKER_LIKE = re.compile(r"<!--[ \t]*snippet:")

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
    return int(MARKER.search(marker_line).group(1))


def extract(path: Path, rel: str) -> tuple[list[Snippet], list[str]]:
    """Split one markdown file into its tagged snippets and any problems."""
    lines = path.read_text().splitlines()
    snippets: list[Snippet] = []
    problems: list[str] = []
    markers: dict[int, str] = {}
    used: set[int] = set()
    i = 0
    while i < len(lines):
        info = lines[i].split("```", 1)
        if MARKER_LIKE.search(lines[i]):
            markers[i + 1] = lines[i]
            i += 1
            continue
        if not lines[i].startswith("```"):
            i += 1
            continue
        words = info[1].split()
        open_line = i + 1
        i += 1
        body: list[str] = []
        while i < len(lines) and not lines[i].startswith("```"):
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
        if j >= 0 and MARKER.search(lines[j]):
            ticket = _ticket(lines[j])
            used.add(j + 1)
        elif j >= 0 and MARKER_LIKE.search(lines[j]):
            problems.append(f"{rel}:{j + 1}: snippet marker without a ticket number")
            used.add(j + 1)
        if len(words) > 1:
            problems.append(f"{rel}:{open_line}: unexpected info string {info[1]!r}")
            continue
        language = words[0]
        if language not in LANGUAGES:
            problems.append(f"{rel}:{open_line}: unknown snippet language {language!r}")
            continue
        snippets.append(Snippet(rel, open_line, language, "\n".join(body) + "\n", ticket))
    for lineno, text in markers.items():
        if lineno in used:
            continue
        if MARKER.search(text):
            problems.append(f"{rel}:{lineno}: snippet marker above no fence")
        else:
            problems.append(f"{rel}:{lineno}: snippet marker without a ticket number")
    return snippets, problems


def _compile_c(snippet: Snippet, root: Path, workdir: Path) -> str | None:
    compiler = shutil.which("gcc" if snippet.language == "c" else "g++")
    if compiler is None:
        return f"{snippet.path}:{snippet.line}: no compiler for {snippet.language}"
    standard = "c99" if snippet.language == "c" else "c++11"
    src = workdir / f"snippet{EXTENSION[snippet.language]}"
    src.write_text(snippet.code)
    cmd = [compiler, f"-std={standard}", "-Werror", "-fsyntax-only", str(src)]
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
    files += sorted((root / "docs").glob("*.md")) if (root / "docs").is_dir() else []
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
