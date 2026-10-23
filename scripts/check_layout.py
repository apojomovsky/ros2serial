#!/usr/bin/env python3
"""Check the repository skeleton: required paths, the rulebook links, no stray proposals.

Exits 1 with one line per problem. Run from anywhere: paths resolve from the
repository root (this file's parent directory).
"""

import os
import re
import sys
from pathlib import Path

REQUIRED_DIRS = [
    "spec/vectors/crc",
    "spec/vectors/rcobs",
    "spec/vectors/varint",
    "spec/vectors/frames",
    "spec/vectors/messages",
    "spec/vectors/session",
    "spec/vectors/time",
    "core/include/r2s",
    "core/src",
    "core/tests",
    "core/fuzz",
    "cpp",
    "gen/r2s_gen",
    "gen/tests",
    "bridge/r2s_bridge",
    "bridge/r2s_bridge_tests",
    "ports/posix",
    "ports/arduino",
    "ports/avr",
    "ports/ch32v",
    "ports/stm32",
    "ports/epic-hal",
    "examples",
    "tests/e2e",
    "tests/harness",
    "tools",
    "scripts",
    "ci",
    ".github/workflows",
    ".githooks",
]
REQUIRED_FILES = [
    "LICENSE",
    "README.md",
    "AGENTS.md",
    "CLAUDE.md",
    "CONTRIBUTING.md",
    ".gitignore",
    "docs/00-design.md",
    "docs/01-testing.md",
]
GITIGNORED = ["scratch/", ".worktrees/"]
# The alias is a link, so a copy cannot drift from the rulebook it stands for.
RULEBOOK_ALIAS = "CLAUDE.md"
RULEBOOK = "AGENTS.md"


def check(root: Path) -> list[str]:
    problems = []
    for d in REQUIRED_DIRS:
        if not (root / d).is_dir():
            problems.append(f"missing directory: {d}")
    for f in REQUIRED_FILES:
        if not (root / f).is_file():
            problems.append(f"missing file: {f}")
    alias = root / RULEBOOK_ALIAS
    if alias.is_symlink():
        # realpath and not resolve: a link that points at itself is a loop
        # that resolve raises on, and the gate must report it, not crash.
        if os.path.realpath(alias) != os.path.realpath(root / RULEBOOK):
            problems.append(f"{RULEBOOK_ALIAS} is not a symlink to {RULEBOOK}")
    elif alias.exists():
        problems.append(f"{RULEBOOK_ALIAS} is not a symlink to {RULEBOOK}")
    agents = root / "AGENTS.md"
    if agents.is_file() and "docs/00-design.md" not in agents.read_text():
        problems.append("AGENTS.md does not link docs/00-design.md")
    contributing = root / "CONTRIBUTING.md"
    if contributing.is_file() and "AGENTS.md" not in contributing.read_text():
        problems.append("CONTRIBUTING.md does not defer to AGENTS.md")
    lic = root / "LICENSE"
    if lic.is_file() and not lic.read_text().startswith("MIT License"):
        problems.append("LICENSE is not the MIT license")
    ignore = root / ".gitignore"
    if ignore.is_file():
        lines = ignore.read_text().splitlines()
        for pattern in GITIGNORED:
            if pattern not in lines:
                problems.append(f".gitignore does not ignore {pattern}")
    for p in sorted(root.glob("*.md")):
        if re.fullmatch(r"proposal-v\d+\.md|engineering-practices\.md", p.name):
            problems.append(f"superseded document still at the root: {p.name}")
    return problems


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    problems = check(root)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
