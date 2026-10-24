#!/usr/bin/env python3
"""Pair a spec change with a vector change, for the rules that cross the wire.

Check 8 of the takeoff ritual. A rule in a section whose heading ends in
`(local)` specifies bytes that never appear on the wire, so a test covers it
and a branch that changes only such rules needs no vector. Every other rule
crosses the wire, and a change to one comes with a change under spec/vectors.

Usage: spec_sync_check.py --base origin/master
Prints one line per problem and exits 1 while any remain.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

SPEC = "spec/protocol.md"
VECTORS = "spec/vectors/"
MARKER = "(local)"
RULE = re.compile(r"^\s*- \[([A-Z]{2,4}-\d+)\] (.*)$")
HEADING = re.compile(r"^(#{1,6})\s+(.*\S)\s*$")


def _rules(text: str) -> dict[str, tuple[bool, str]]:
    """Every rule in document order, with whether a local section holds it."""
    rules: dict[str, tuple[bool, str]] = {}
    stack: list[tuple[int, bool]] = []
    for line in text.splitlines():
        heading = HEADING.match(line)
        if heading:
            level, title = len(heading.group(1)), heading.group(2)
            while stack and stack[-1][0] >= level:
                stack.pop()
            local = (stack[-1][1] if stack else False) or title.endswith(MARKER)
            stack.append((level, local))
            continue
        rule = RULE.match(line)
        if rule:
            rules[rule.group(1)] = (stack[-1][1] if stack else False, rule.group(2))
    return rules


def _show(root: Path, ref: str) -> str:
    result = subprocess.run(["git", "show", f"{ref}:{SPEC}"], cwd=root, capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else ""


def _changed(root: Path, base: str) -> list[str]:
    result = subprocess.run(["git", "diff", "--name-only", f"{base}...HEAD"], cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"spec sync: cannot compare {base} with HEAD: {result.stderr.strip()}")
    return [line for line in result.stdout.splitlines() if line]


def check(root: Path, base: str) -> list[str]:
    changed = _changed(root, base)
    spec_changed = SPEC in changed
    # A .gitkeep carries no vector, so adding or removing one changes none.
    vectors_changed = any(path.startswith(VECTORS) and not path.endswith("/.gitkeep") for path in changed)
    problems = []
    if spec_changed and not vectors_changed:
        old = _rules(_show(root, base))
        new = _rules((root / SPEC).read_text(encoding="utf-8"))
        # A rule that is added, removed, reworded or moved between sections is
        # a change; only the wire ones need a vector to show them.
        wire = [rule for rule, entry in new.items() if not entry[0] and old.get(rule) != entry]
        wire += [rule for rule, entry in old.items() if not entry[0] and (rule not in new or new[rule][0])]
        if wire:
            problems.append(f"{SPEC} changed without spec/vectors changes: {', '.join(wire)} crosses the wire")
    if vectors_changed and not spec_changed:
        problems.append(f"spec/vectors changed without {SPEC} changes")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="origin/master")
    args = ap.parse_args()
    problems = check(Path.cwd(), args.base)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
