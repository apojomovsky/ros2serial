#!/usr/bin/env python3
"""Pick the next ticket: open, unblocked, unclaimed, and in an area no open PR is working on.

A draft or open PR claims every issue its body closes (Closes, Fixes or
Resolves #N). Its areas are its own area:* labels plus those of the issues
it closes. With --agent, the PRs whose body says `agent: <id>` don't block
that agent's areas, so it can stack its next ticket on its own open PRs.
Issues labelled needs-hardware or dispatch-only belong to the maintainer.
Two PRs claiming one issue is a race: the older PR keeps it.

Usage:
  next_ticket.py [--repo owner/name] [--agent ID] [--why]     read issues and PRs with `gh api`
  next_ticket.py --from-json ISSUES PRS [--agent ID] [--why]  read them from files (REST JSON)
"""

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field

DEPENDS = re.compile(r"^Depends on #(\d+)", re.MULTILINE)
CLOSES = re.compile(r"\b(?:closes|fixes|resolves) #(\d+)", re.IGNORECASE)
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


def select(issues: list[dict], prs: list[dict], agent: str | None = None) -> Report:
    issues = [i for i in issues if "pull_request" not in i]
    by_number = {i["number"]: i for i in issues}
    claims: dict[int, list[dict]] = defaultdict(list)
    busy: dict[str, int] = {}
    for pr in sorted(prs, key=lambda p: p.get("created_at", "")):
        closed = [int(n) for n in CLOSES.findall(pr.get("body") or "")]
        for n in closed:
            claims[n].append(pr)
        areas = {a for a in _labels(pr) if a.startswith("area:")}
        for n in closed:
            areas |= {a for a in _labels(by_number.get(n, {})) if a.startswith("area:")}
        if agent and f"agent: {agent}" in (pr.get("body") or ""):
            continue
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
            waiting = [
                d
                for d in map(int, DEPENDS.findall(issue.get("body") or ""))
                if by_number.get(d, {}).get("state") != "closed"
            ]
            if waiting:
                reason = "waits on " + ", ".join(f"#{d} (open or unknown)" for d in waiting)
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
    ap.add_argument("--agent", help="your agent ID (<runtime>@<host>): your own open PRs do not block your areas")
    ap.add_argument("--why", action="store_true", help="also list skipped tickets and why")
    args = ap.parse_args()
    if args.from_json:
        issues, prs = (json.load(open(p, encoding="utf-8")) for p in args.from_json)
    else:
        issues = _gh(f"repos/{args.repo}/issues?state=all&per_page=100")
        prs = _gh(f"repos/{args.repo}/pulls?state=open&per_page=100")
    report = select(issues, prs, args.agent)
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
