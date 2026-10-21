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
from pathlib import Path


def _without_code(body: str) -> str:
    body = re.sub(r"^(```|~~~).*?^\1[^\n]*$", "", body, flags=re.S | re.M)
    body = re.sub(r"^(?:    |\t).*$", "", body, flags=re.M)
    return re.sub(r"`[^`\n]*`", "", body)


def check(body: str, full: bool) -> list[str]:
    problems = []
    prose = _without_code(body)
    if "\\n" in prose or "\\r" in prose:
        problems.append("PR body contains a literal \\n or \\r: write it to a file or a heredoc with real newlines")
    if full:
        if not re.search(r"\b(?:closes|fixes|resolves) #\d+", body, re.I):
            problems.append("PR body has no Closes #N")
        if not re.search(r"^## Review\b", body, re.M):
            problems.append("PR body has no ## Review section")
        if not re.search(r"^(?:```|~~~)", body, re.M):
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
        try:
            body = Path(args.file).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as e:
            print(f"PR body file {args.file} cannot be read as UTF-8 text: {e}")
            return 1
    else:
        body = os.environ.get(args.env, "")
    problems = check(body, args.full)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
