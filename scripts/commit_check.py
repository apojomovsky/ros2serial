#!/usr/bin/env python3
"""Check commit messages and PR titles against the commit hygiene rules in AGENTS.md.

Usage:
  commit_check.py --file <path>          one message (the commit-msg hook)
  commit_check.py --range <base..head>   every non-merge commit in a range
  commit_check.py --title <text>         a PR title
  commit_check.py --title <text> --covers-range <base..head>
                                           the title must cover what the commits declare

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


# The type and the breaking marker a subject declares.
def _declared(subject: str) -> tuple[str, bool]:
    """The type and the breaking marker a subject declares.

    The `!` of a breaking change comes after the scope, so it is read from
    the part before the colon.
    """
    m = SUBJECT.match(subject)
    if not m:
        return "", False
    head = m.group(0).split(":", 1)[0]
    return head.split("(", 1)[0].split("!", 1)[0], "!" in head


# How much a type says. A title that says less than a commit hides the
# commit's type from the changelog and the version bump.
_STRENGTH = {
    "chore": 0,
    "docs": 1,
    "test": 1,
    "build": 1,
    "ci": 1,
    "refactor": 2,
    "revert": 2,
    "perf": 3,
    "fix": 4,
    "feat": 5,
}


def check_title_covers(title: str, subjects: list[str]) -> list[str]:
    """The title is the subject that lands, so it must carry the strongest type.

    A PR holding a `feat` commit but titled `docs(...)` reaches neither the
    changelog nor the version bump, and a `!` on a commit is lost with the
    commit, so the title must carry it.
    """
    problems = []
    title_kind, title_breaking = _declared(title)
    if not title_kind:
        return problems
    for subject in subjects:
        kind, breaking = _declared(subject)
        if not kind:
            continue
        if _STRENGTH.get(kind, 0) > _STRENGTH.get(title_kind, 0):
            problems.append(
                f"the title is {title_kind!r} but a commit is {kind!r}: the title is the "
                f"subject that lands, so it must carry the strongest type"
            )
        if breaking and not title_breaking:
            problems.append("a commit is a breaking change and the title is not: the title must carry the '!'")
    return problems


def _range_messages(rev_range: str) -> list[tuple[str, str]]:
    # A merge commit is not part of a PR's work: the PR is squashed, so its
    # merge commit is never what lands. A merge commit on a branch that was
    # pushed before the rule changed stays there, so the range check leaves
    # them alone rather than failing a PR over history it cannot change.
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
    # The range the title must cover goes with the title, so it is its own
    # argument rather than part of the mutually exclusive group.
    ap.add_argument("--covers-range", help="the range of commits the title must cover")
    args = ap.parse_args()

    if args.file:
        with open(args.file, encoding="utf-8") as f:
            problems = check_message(f.read())
    elif args.title is not None:
        problems = [f"PR title: {p}" for p in check_title(args.title)]
        if args.covers_range:
            subjects = [msg.splitlines()[0] for _, msg in _range_messages(args.covers_range)]
            problems += [f"PR title: {p}" for p in check_title_covers(args.title.strip(), subjects)]
    else:
        problems = [f"{sha}: {p}" for sha, msg in _range_messages(args.range) for p in check_message(msg)]
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
