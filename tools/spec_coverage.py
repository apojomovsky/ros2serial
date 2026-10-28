#!/usr/bin/env python3
"""Check that every rule of spec/protocol.md is covered by a vector or a test.

A rule is covered when a vector file lists its ID under `covers`, or when a test
has a marker line naming it: `covers: FRM-4, FRM-5` on its own line, in a
comment (`#`, `//`, `/* */`) or, in Python, in a docstring. Only the test roots
below are scanned; vendor/, fixtures/ and must_fail/ directories are skipped.

A rule whose implementation lands in a later ticket is listed in
tools/spec_pending.txt with that ticket. A listed rule fails the check once it
is covered, and with --base only rules new to the spec since that commit may
join the list, so a rule that had coverage can never be parked there. Retired
rules need no coverage and may not be claimed.

Usage: spec_coverage.py [--base REF]
Exits 1 with one line per problem.
"""

import argparse
import ast
import io
import re
import subprocess
import sys
import tokenize
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import yaml

DEFINITION = re.compile(r"^\s*- \[([A-Z]{2,4}-\d+)\] (.*)$", re.MULTILINE)
ID = re.compile(r"^[A-Z]{2,4}-\d+$")
COMMENT_TEXT = re.compile(r"^\s*(?:#+|//+|/\*+|\*+)?\s*(.*?)\s*(?:\*+/)?\s*$")
MARKER_START = re.compile(r"^covers:", re.IGNORECASE)
DESIGN_TICKET = re.compile(r"^\| ([A-Z]+\d+) \|", re.MULTILINE)
TEST_ROOTS = (
    "core/tests",
    "cpp/tests",
    "gen/tests",
    "bridge/r2s_bridge/test",
    "bridge/r2s_bridge_tests",
    "ports/posix/tests",
    "tests/e2e",
)
C_SOURCES = {".c", ".h", ".cpp", ".hpp", ".cc"}
SKIPPED_DIRS = {"vendor", "fixtures", "must_fail", "__pycache__"}
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


def _marker(text: str) -> list[str] | None | str:
    """The IDs a marker names, None if the text is no marker, or the text itself if it is malformed."""
    if not MARKER_START.match(text):
        return None
    tokens = [t.strip("[]") for t in re.split(r"[,\s]+", text[len("covers:") :]) if t.strip("[]")]
    if not tokens or not all(ID.match(t) for t in tokens) or not text.startswith("covers:"):
        return text
    return tokens


def _c_comment_lines(source: str):
    for lineno, line in enumerate(source.splitlines(), 1):
        if line.lstrip().startswith(("//", "/*", "*")):
            yield lineno, COMMENT_TEXT.match(line).group(1)


def _python_comment_lines(source: str):
    for tok in tokenize.generate_tokens(io.StringIO(source).readline):
        if tok.type == tokenize.COMMENT:
            yield tok.start[0], COMMENT_TEXT.match(tok.string).group(1)
    for node in ast.walk(ast.parse(source)):
        body = getattr(node, "body", None)
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and body:
            first = body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant):
                if isinstance(first.value.value, str):
                    for i, line in enumerate(first.value.value.splitlines()):
                        yield first.value.lineno + i, line.strip()


def _test_sources(root: Path):
    for top in TEST_ROOTS:
        for path in sorted((root / top).rglob("*")):
            parts = set(path.relative_to(root).parts[:-1])
            if path.is_file() and not parts & SKIPPED_DIRS and (path.suffix == ".py" or path.suffix in C_SOURCES):
                yield path.relative_to(root).as_posix(), path


def _git_show(root: Path, ref: str, path: str) -> str | None:
    result = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=root, capture_output=True)
    if result.returncode != 0:
        return ""
    try:
        return result.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return None


def check(root: Path, base: str | None = None) -> Report:
    report = Report()
    try:
        spec_text = (root / "spec/protocol.md").read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        # Without readable rules there is nothing to cover; name the spec.
        report.problems.append(f"spec/protocol.md: unreadable ({str(e).splitlines()[0]})")
        return report
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

    def claim(rule: str, where: str) -> None:
        if rule not in defined:
            report.problems.append(f"{where}: names {rule}, which spec/protocol.md does not define")
        elif rule in retired:
            report.problems.append(f"{where}: names {rule}, which is retired")
        else:
            report.covered[rule].append(where)

    for path in sorted((root / "spec/vectors").rglob("*.yaml")):
        rel = path.relative_to(root).as_posix()
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        except (UnicodeDecodeError, yaml.YAMLError) as e:
            report.problems.append(f"{rel}: unreadable ({str(e).splitlines()[0]})")
            continue
        covers = doc.get("covers") if isinstance(doc, dict) else None
        if not isinstance(covers, list) or not all(isinstance(c, str) for c in covers):
            report.problems.append(f"{rel}: covers must be a list of spec IDs")
            continue
        for rule in covers:
            claim(rule, rel)

    for rel, path in _test_sources(root):
        try:
            source = path.read_text(encoding="utf-8")
            lines = list(_python_comment_lines(source) if path.suffix == ".py" else _c_comment_lines(source))
        except (UnicodeDecodeError, SyntaxError, tokenize.TokenError) as e:
            report.problems.append(f"{rel}: unreadable ({str(e).splitlines()[0]})")
            continue
        for lineno, text in sorted(lines):
            found = _marker(text)
            if isinstance(found, str):
                report.problems.append(f"{rel}:{lineno}: malformed covers marker: {found!r}")
            for rule in found if isinstance(found, list) else []:
                claim(rule, f"{rel}:{lineno}")

    design = root / "docs/00-design.md"
    tickets = set(DESIGN_TICKET.findall(design.read_text(encoding="utf-8"))) if design.is_file() else None
    base_text = _git_show(root, base, "spec/protocol.md") if base else ""
    base_rules = set(DEFINITION.findall(base_text or ""))
    base_rules = {r for r, _ in base_rules}
    base_pending_text = _git_show(root, base, PENDING) if base else ""
    base_pending_text = base_pending_text or ""
    base_pending = {f[0] for f in (ln.split("#", 1)[0].split() for ln in base_pending_text.splitlines()) if f}
    if not base_pending_text:
        base = None  # the PR that introduces the list fills it; from then on it only shrinks
    listed = set()  # a rejected pending line already explains its rule; no second line for it
    pending_file = root / PENDING
    if pending_file.is_file():
        for lineno, line in enumerate(pending_file.read_text(encoding="utf-8").splitlines(), 1):
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            where = f"{PENDING}:{lineno}"
            rule = fields[0]
            listed.add(rule)
            if len(fields) < 2:
                report.problems.append(f"{where}: expected `<ID> <ticket>`")
            elif rule not in defined:
                report.problems.append(f"{where}: {rule} is not defined in spec/protocol.md")
            elif rule in retired:
                report.problems.append(f"{where}: {rule} is retired; remove it")
            elif rule in report.pending:
                report.problems.append(f"{where}: {rule} is listed twice")
            elif rule in report.covered:
                report.problems.append(f"{where}: {rule} is covered now ({report.covered[rule][0]}); remove it")
            elif tickets is not None and fields[1] not in tickets:
                report.problems.append(f"{where}: {fields[1]} is not a ticket in docs/00-design.md")
            elif base and rule in base_rules and rule not in base_pending:
                report.problems.append(
                    f"{where}: {rule} was already a rule at {base}; cover it instead of listing it as pending"
                )
            else:
                report.pending[rule] = fields[1]

    for rule in report.rules:
        if rule not in report.covered and rule not in report.pending and rule not in listed:
            report.problems.append(
                f"{rule} has no vector or test: add `covers: {rule}` to one, or, if its implementation "
                f"is a later ticket, list it in {PENDING} with that ticket"
            )
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", help="the commit the pending list may only shrink against (a PR's base)")
    args = ap.parse_args()
    report = check(Path(__file__).resolve().parent.parent, args.base)
    for p in report.problems:
        print(p)
    print(report.summary())
    return 1 if report.problems else 0


if __name__ == "__main__":
    sys.exit(main())
