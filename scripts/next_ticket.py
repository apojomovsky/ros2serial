#!/usr/bin/env python3
"""Pick the next ticket: open, unblocked, unclaimed, and in an area no open PR is working on.

A draft or open PR claims every issue its body closes (Closes, Fixes or
Resolves #N, or the issue URL). Its areas are its own area:* labels plus those of the issues
it closes. Tickets are not stacked, so an agent's own open PR blocks its
areas like anyone's.
Issues labelled needs-hardware or dispatch-only belong to the maintainer.
Two PRs claiming one issue is a race: the older PR keeps it.

Usage:
  next_ticket.py [--repo owner/name] [--why]     read issues and PRs with `gh api`
  next_ticket.py --from-json ISSUES PRS [--why]  read them from files (REST JSON)
"""

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

DEPENDS_LINE = re.compile(r"^.*\bdepends on\b.*$", re.IGNORECASE | re.MULTILINE)
ISSUE_REF = re.compile(r"(?<![\w/])#(\d+)\b")
CLOSES = re.compile(
    r"\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?):?\s+(?:https://github\.com/[^/\s]+/[^/\s]+/issues/|#)(\d+)\b",
    re.IGNORECASE,
)
MAINTAINER = {
    "needs-hardware": "needs hardware: the maintainer runs it",
    "dispatch-only": "maintainer only (dispatch-only)",
}


@dataclass
class Report:
    available: list[dict] = field(default_factory=list)
    skipped: dict[int, str] = field(default_factory=dict)
    races: dict[int, list[int]] = field(default_factory=dict)


def _labels(item: dict) -> set[str]:
    return {lbl["name"] for lbl in item.get("labels", [])}


def _depends(body: str) -> list[int]:
    return [int(n) for line in DEPENDS_LINE.findall(body) for n in ISSUE_REF.findall(line)]


def select(issues: list[dict], prs: list[dict]) -> Report:
    # Merged or closed PRs show up in the issue list too, and a dependency on one counts.
    state = {i["number"]: i["state"] for i in issues}
    issues = [i for i in issues if "pull_request" not in i]
    by_number = {i["number"]: i for i in issues}
    claims: dict[int, list[dict]] = defaultdict(list)
    busy: dict[str, int] = {}
    for pr in sorted(prs, key=lambda p: p.get("created_at", "")):
        closed = list(dict.fromkeys(int(n) for n in CLOSES.findall(pr.get("body") or "")))
        for n in closed:
            claims[n].append(pr)
        areas = {a for a in _labels(pr) if a.startswith("area:")}
        for n in closed:
            areas |= {a for a in _labels(by_number.get(n, {})) if a.startswith("area:")}
        for a in areas:
            busy.setdefault(a, pr["number"])

    report = Report()
    for n, owners in claims.items():
        if len(owners) > 1:
            report.races[n] = [p["number"] for p in owners]
    for issue in sorted(issues, key=lambda i: i["number"]):
        if issue["state"] != "open":
            continue
        n, labels = issue["number"], _labels(issue)
        reason = next((MAINTAINER[label] for label in MAINTAINER if label in labels), None)
        if reason is None and claims.get(n):
            reason = f"claimed by PR #{claims[n][0]['number']}"
        if reason is None:
            waiting = [d for d in dict.fromkeys(_depends(issue.get("body") or "")) if state.get(d) != "closed"]
            if waiting:
                reason = "waits on " + ", ".join(f"#{d} ({'open' if d in state else 'not found'})" for d in waiting)
        if reason is None:
            clash = sorted(a for a in labels if a in busy)
            if clash:
                reason = ", ".join(f"{a} busy with PR #{busy[a]}" for a in clash)
        if reason is None:
            report.available.append(issue)
        else:
            report.skipped[n] = reason
    return report


def _gh(path: str) -> list[dict]:
    # --slurp wraps every page's array in one outer array.
    out = subprocess.run(
        ["gh", "api", "--paginate", "--slurp", path], capture_output=True, text=True, check=True
    ).stdout
    return [item for page in json.loads(out) for item in page]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default="apojomovsky/ros2serial")
    ap.add_argument("--from-json", nargs=2, metavar=("ISSUES", "PRS"))
    ap.add_argument("--why", action="store_true", help="also list skipped tickets and why")
    args = ap.parse_args()
    if args.from_json:
        issues, prs = (json.loads(Path(p).read_text(encoding="utf-8")) for p in args.from_json)
    else:
        issues = _gh(f"repos/{args.repo}/issues?state=all&per_page=100")
        prs = _gh(f"repos/{args.repo}/pulls?state=open&per_page=100")
    report = select(issues, prs)
    for n, owners in report.races.items():
        print(f"race on #{n}: PRs {', '.join(f'#{p}' for p in owners)}; the older one keeps it, the others close")
    for issue in report.available:
        areas = ", ".join(sorted(a for a in _labels(issue) if a.startswith("area:")))
        print(f"#{issue['number']} {issue['title']} [{areas}]")
    if not report.available:
        print("no ticket is available")
    if args.why:
        for n, reason in sorted(report.skipped.items()):
            print(f"skip #{n}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
