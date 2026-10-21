#!/usr/bin/env python3
"""List the comment blocks a branch adds and check the mechanical prose rules.

The rules come from AGENTS.md (Expression conventions): a block of at most 8
lines, no decoration, no change narrative, no em-dashes, and no TODO or FIXME
without a reason. Whether a comment is worth having stays the author's call;
the listing is there to read.

Usage:
  prose_check.py [--base REF]           list added comment blocks (default origin/master)
  prose_check.py --verify [--base REF]  exit 1 on any rule violation
"""

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field

MAX_BLOCK = 8
HASH_SUFFIXES = (".py", ".sh", ".yml", ".yaml", ".toml", ".env", ".cfg")
HASH_NAMES = ("Makefile", "Dockerfile", ".gitignore", ".dockerignore")
C_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".ino")
DECORATION = re.compile(r"@(file|brief)\b|^[-=*#~_/ ]{4,}$")
NARRATIVE = re.compile(r"\b(fixed (by|in|the)|this (pr|commit|patch)|previously|was changed|we now|now we)\b", re.I)
TODO = re.compile(r"\b(TODO|FIXME)\b(?!\s*[:(]\s*\S.{8,})")
EM_DASH = "\u2014"


@dataclass
class Block:
    path: str
    line: int
    lines: list[str] = field(default_factory=list)


def _style(path: str) -> str | None:
    name = path.rsplit("/", 1)[-1]
    if path.startswith(".githooks/") or name in HASH_NAMES or name.endswith(HASH_SUFFIXES):
        return "hash"
    if name.endswith(C_SUFFIXES):
        return "c"
    if name.endswith(".md"):
        return "prose"
    return None


def _is_comment(style: str, text: str) -> bool:
    s = text.strip()
    if style == "hash":
        return s.startswith("#") and not s.startswith("#!")
    if style == "c":
        return s.startswith(("//", "/*", "*"))
    return False


def _text(style: str, line: str) -> str:
    s = line.strip()
    if style == "c":
        s = re.sub(r"^(//+|/\*+|\*+/?)", "", s)
        s = re.sub(r"\*+/$", "", s)
    else:
        s = s.lstrip("#")
    return s.strip()


def added_lines(diff: str):
    """Yield (path, line number, text) for every added line of a unified diff."""
    path, lineno = None, 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            target = raw[4:]
            path = target[2:] if target.startswith("b/") else None
        elif raw.startswith("@@"):
            m = re.match(r"@@ -\S+ \+(\d+)", raw)
            lineno = int(m.group(1)) if m else 0
        elif path and raw.startswith("+"):
            yield path, lineno, raw[1:]
            lineno += 1
        elif path and raw.startswith(" "):
            lineno += 1


def added_comment_blocks(diff: str) -> list[Block]:
    blocks: list[Block] = []
    for path, lineno, text in added_lines(diff):
        style = _style(path)
        if style not in ("hash", "c") or not _is_comment(style, text):
            continue
        last = blocks[-1] if blocks else None
        if last and last.path == path and last.line + len(last.lines) == lineno:
            last.lines.append(text)
        else:
            blocks.append(Block(path, lineno, [text]))
    return blocks


def verify(diff: str) -> list[str]:
    problems = []
    for b in added_comment_blocks(diff):
        where = f"{b.path}:{b.line}"
        if len(b.lines) > MAX_BLOCK:
            problems.append(f"{where}: comment block of {len(b.lines)} lines; at most {MAX_BLOCK}")
        style = _style(b.path)
        for i, line in enumerate(b.lines):
            text, at = _text(style, line), f"{b.path}:{b.line + i}"
            if DECORATION.search(text) or DECORATION.search(line.strip()):
                problems.append(f"{at}: decoration (@file, @brief or separator art) in a comment")
            if NARRATIVE.search(text):
                problems.append(f"{at}: change narrative in a comment; it belongs in the PR")
            if TODO.search(text):
                problems.append(f"{at}: TODO or FIXME without a reason")
    for path, lineno, text in added_lines(diff):
        style = _style(path)
        if EM_DASH in text and (style == "prose" or (style in ("hash", "c") and _is_comment(style, text))):
            problems.append(f"{path}:{lineno}: em-dash; use a comma, a colon or a period")
    return problems


def branch_diff(base: str) -> str:
    return subprocess.run(
        ["git", "diff", "--no-color", "-U0", f"{base}...HEAD"], capture_output=True, text=True, check=True
    ).stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="origin/master")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    diff = branch_diff(args.base)
    if args.verify:
        problems = verify(diff)
        for p in problems:
            print(p)
        return 1 if problems else 0
    for b in added_comment_blocks(diff):
        print(f"== {b.path}:{b.line}")
        print("\n".join(b.lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
