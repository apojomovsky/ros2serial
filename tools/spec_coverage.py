#!/usr/bin/env python3
"""Check that every rule of spec/protocol.md is covered by a vector or a test.

A rule is covered when a vector file lists its ID under `covers`, or when a test
source has a marker line naming it: `covers: FRM-4, FRM-5` on its own line, as a
comment (`#`, `//`, `/* */`) or inside a docstring. Test sources are the .py,
.c, .h, .cpp, .hpp and .cc files under a tests/ directory; vendor/ and
fixtures/ directories are skipped.

A rule whose implementation has not landed yet is listed in
tools/spec_pending.txt with the ticket that will cover it. The list only
shrinks: a pending rule that becomes covered must leave it. Retired rules need
no coverage and may not be claimed.

Exits 1 with one line per problem.
"""

import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import yaml

DEFINITION = re.compile(r"^\s*- \[([A-Z]{2,4}-\d+)\] (.*)$", re.MULTILINE)
ID = re.compile(r"^[A-Z]{2,4}-\d+$")
MARKER = re.compile(r"^\s*(?:#+|//+|/\*+|\*+)?\s*covers:\s*(.*?)\s*(?:\*+/)?\s*$")
SOURCES = {".py", ".c", ".h", ".cpp", ".hpp", ".cc"}
SKIPPED_DIRS = {"vendor", "fixtures", "__pycache__"}
PENDING = "tools/spec_pending.txt"


@dataclass
class Report:
    rules: list[str] = field(default_factory=list)
    covered: dict[str, list[str]] = field(default_factory=lambda: defaultdict(list))
    pending: dict[str, str] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)

    def summary(self) -> str:
        done = sum(1 for r in self.rules if r in self.covered)
        return f"spec coverage: {done} of {len(self.rules)} rules covered, {len(self.pending)} pending"


def _marker_ids(line: str) -> list[str] | None:
    """The IDs a marker line names, or None if the line is not a marker."""
    m = MARKER.match(line)
    if not m:
        return None
    tokens = [t.strip("[]") for t in re.split(r"[,\s]+", m.group(1)) if t.strip("[]")]
    if not tokens or not all(ID.match(t) for t in tokens):
        return None
    return tokens


def _test_sources(root: Path):
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        parts = set(rel.parts[:-1])
        if path.suffix in SOURCES and "tests" in parts and not parts & SKIPPED_DIRS and path.is_file():
            yield rel.as_posix(), path


def check(root: Path) -> Report:
    report = Report()
    spec_text = (root / "spec/protocol.md").read_text(encoding="utf-8")
    counts: dict[str, int] = defaultdict(int)
    retired = set()
    for rule, text in DEFINITION.findall(spec_text):
        counts[rule] += 1
        if text.strip().startswith("Retired."):
            retired.add(rule)
        elif counts[rule] == 1:
            report.rules.append(rule)
    for rule, n in counts.items():
        if n > 1:
            report.problems.append(f"spec/protocol.md: {rule} is defined {n} times")
    defined = set(counts)

    def claim(rule: str, where: str, location: str) -> None:
        if rule not in defined:
            report.problems.append(f"{location}: names {rule}, which spec/protocol.md does not define")
        elif rule in retired:
            report.problems.append(f"{location}: names {rule}, which is retired")
        else:
            report.covered[rule].append(where)

    for path in sorted((root / "spec/vectors").rglob("*.yaml")):
        rel = path.relative_to(root).as_posix()
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        for rule in (doc or {}).get("covers") or []:
            claim(rule, rel, rel)
    for rel, path in _test_sources(root):
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for rule in _marker_ids(line) or []:
                claim(rule, f"{rel}:{lineno}", f"{rel}:{lineno}")

    pending_file = root / PENDING
    if pending_file.is_file():
        for lineno, line in enumerate(pending_file.read_text(encoding="utf-8").splitlines(), 1):
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            where = f"{PENDING}:{lineno}"
            if len(fields) < 2:
                report.problems.append(f"{where}: expected `<ID> <ticket>`")
            elif fields[0] not in defined:
                report.problems.append(f"{where}: {fields[0]} is not defined in spec/protocol.md")
            elif fields[0] in report.covered:
                report.problems.append(f"{where}: {fields[0]} is covered now; remove it")
            else:
                report.pending[fields[0]] = fields[1]

    for rule in report.rules:
        if rule not in report.covered and rule not in report.pending:
            report.problems.append(
                f"{rule} has no vector or test: add `covers: {rule}` to one, or list it in {PENDING} with its ticket"
            )
    return report


def main() -> int:
    report = check(Path(__file__).resolve().parent.parent)
    for p in report.problems:
        print(p)
    print(report.summary())
    return 1 if report.problems else 0


if __name__ == "__main__":
    sys.exit(main())
