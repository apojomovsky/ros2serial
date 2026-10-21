#!/usr/bin/env python3
"""Check a PR body: real newlines always; with --full, also the ritual's sections.

CI (pr-body.yml) runs the newline check on every PR, since a body written with
an inline "\\n" string renders as one line. The takeoff ritual runs --full: a
Closes #N line, a ## Review section and the failing run as a fenced block.

Usage:
  pr_body_check.py [--full] --file <path>
  pr_body_check.py [--full] --env <VAR>    read the body from an environment variable
"""

import argparse
import os
import re
import sys


def _without_code(body: str) -> str:
    return re.sub(r"`[^`\n]*`", "", re.sub(r"```.*?```", "", body, flags=re.S))


def check(body: str, full: bool) -> list[str]:
    problems = []
    prose = _without_code(body)
    if "\\n" in prose or "\\r" in prose:
        problems.append("PR body contains a literal \\n or \\r: write it to a file or a heredoc with real newlines")
    if full:
        if not re.search(r"\b(?:Closes|Fixes|Resolves) #\d+", body):
            problems.append("PR body has no Closes #N")
        if not re.search(r"^## Review\b", body, re.M):
            problems.append("PR body has no ## Review section")
        if not re.search(r"^```", body, re.M):
            problems.append("PR body has no failing test run (a fenced code block from the first commit)")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--full", action="store_true")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--file")
    src.add_argument("--env")
    args = ap.parse_args()
    if args.file:
        with open(args.file, encoding="utf-8") as f:
            body = f.read()
    else:
        body = os.environ.get(args.env, "")
    problems = check(body, args.full)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
