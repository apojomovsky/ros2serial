#!/usr/bin/env python3
"""Check commit messages and PR titles against the commit hygiene rules in AGENTS.md.

Usage:
  commit_check.py --file <path>        one message (the commit-msg hook, #8)
  commit_check.py --range <base..head> every non-merge commit in a range
  commit_check.py --title <text>       a PR title (squash merges use it as the subject)

Exits 1 with one line per problem.
"""

import argparse
import re
import subprocess
import sys

TYPES = ("feat", "fix", "docs", "test", "ci", "build", "chore", "refactor", "perf", "revert")
SUBJECT = re.compile(rf"^(?:{'|'.join(TYPES)})(?:\([a-z0-9][a-z0-9-]*\))?!?: \S")
SUBJECT_HELP = f"subject must be '<type>(<scope>): <summary>' with type one of {', '.join(TYPES)}"
MAX_LINES = 3


def _lines(message: str) -> list[str]:
    # Git drops '#' comment lines and trailing blank lines before storing a message.
    lines = [ln for ln in message.splitlines() if not ln.startswith("#")]
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def _trailers(message: str) -> str:
    result = subprocess.run(
        ["git", "interpret-trailers", "--parse", "--no-divider"],
        input=message,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def check_subject(subject: str) -> list[str]:
    problems = []
    if not SUBJECT.match(subject):
        problems.append(f"{SUBJECT_HELP}: {subject!r}")
    if "\u2014" in subject:
        problems.append("em-dash in the subject; use a comma, a colon or a period")
    return problems


def check_message(message: str) -> list[str]:
    lines = _lines(message)
    if not lines:
        return ["empty message"]
    problems = check_subject(lines[0])
    body = "\n".join(lines[1:])
    if "\u2014" in body:
        problems.append("em-dash in the body; use a comma, a colon or a period")
    if len(lines) > MAX_LINES:
        problems.append(f"message has {len(lines)} lines; at most {MAX_LINES} lines allowed")
    if re.search(r"^BREAKING[ -]CHANGE:", body, re.MULTILINE):
        problems.append("mark breaking changes with ! in the subject, not a BREAKING CHANGE footer")
    elif _trailers("\n".join(lines)):
        problems.append("trailer found (Co-Authored-By, Signed-off-by and the like are not allowed)")
    return problems


def check_title(title: str) -> list[str]:
    if "\n" in title.strip():
        return ["PR title must be a single line"]
    return check_subject(title.strip())


def _range_messages(rev_range: str) -> list[tuple[str, str]]:
    shas = subprocess.run(
        ["git", "rev-list", "--no-merges", rev_range], capture_output=True, text=True, check=True
    ).stdout.split()
    out = []
    for sha in shas:
        msg = subprocess.run(
            ["git", "log", "-1", "--format=%B", sha], capture_output=True, text=True, check=True
        ).stdout
        out.append((sha[:7], msg))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--file")
    group.add_argument("--range")
    group.add_argument("--title")
    args = ap.parse_args()

    if args.file:
        with open(args.file, encoding="utf-8") as f:
            problems = check_message(f.read())
    elif args.title is not None:
        problems = [f"PR title: {p}" for p in check_title(args.title)]
    else:
        problems = [f"{sha}: {p}" for sha, msg in _range_messages(args.range) for p in check_message(msg)]
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
