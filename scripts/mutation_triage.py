#!/usr/bin/env python3
"""Merge normalized mutation results into one triage report.

Mull and mutmut speak different formats, so scripts/run-mutation.sh
normalizes each into {"component", "mutants": [{id, file, line, mutator,
outcome}]} before calling this. This script scores each component against
the docs/01-testing.md section 6 floors, lists the surviving mutants that
still need a test, and records the acknowledged equivalents with their
reasons. A low score never fails: the nightly report informs, it gates
nothing. Malformed input does fail, so a broken nightly run is loud.
"""

import argparse
import json
import sys
from pathlib import Path

import yaml

# docs/01-testing.md, section 6: the nightly mutation floors per component.
FLOORS = {"core": 85, "cpp": 75, "gen": 80, "bridge": 70}


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        sys.exit(f"mutation_triage: result file not found: {path}")
    except json.JSONDecodeError as exc:
        sys.exit(f"mutation_triage: result file is not valid JSON: {path}: {exc}")


def load_equivalent(path):
    try:
        entries = yaml.safe_load(Path(path).read_text(encoding="utf-8")) or []
    except FileNotFoundError:
        sys.exit(f"mutation_triage: equivalent file not found: {path}")
    known = {}
    for entry in entries:
        known[entry["mutant"]] = entry["reason"]
    return known


def check_results(results):
    for result in results:
        if result.get("component") not in FLOORS:
            sys.exit(f"mutation_triage: unknown component: {result.get('component')!r}")
        for mutant in result.get("mutants", []):
            if mutant.get("outcome") not in ("survived", "killed"):
                sys.exit(f"mutation_triage: unknown outcome for {mutant.get('id')!r}")


def score(mutants):
    killed = sum(1 for mutant in mutants if mutant["outcome"] == "killed")
    total = len(mutants)
    percent = "n/a" if total == 0 else f"{100 * killed // total}%"
    return killed, total, percent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mull", required=True)
    parser.add_argument("--mutmut", required=True)
    parser.add_argument("--equivalent", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    results = [load_json(args.mull), load_json(args.mutmut)]
    check_results(results)
    known = load_equivalent(args.equivalent)
    by_component = {result["component"]: result.get("mutants", []) for result in results}
    lines = ["# Mutation triage", ""]
    lines += ["| Component | Killed | Total | Score | Floor |", "| --- | --- | --- | --- | --- |"]
    for component in ("core", "cpp", "gen", "bridge"):
        killed, total, percent = score(by_component.get(component, []))
        mark = "ok" if total and 100 * killed // total >= FLOORS[component] else ("below floor" if total else "no data")
        lines.append(f"| {component} | {killed} | {total} | {percent} | {FLOORS[component]}% | {mark} |")
    survivors = [m for mutants in by_component.values() for m in mutants if m["outcome"] == "survived"]
    fresh = [m for m in survivors if m["id"] not in known]
    lines += ["", "## Acknowledged equivalents", ""]
    if known:
        for mutant, reason in sorted(known.items()):
            status = "seen" if any(m["id"] == mutant for m in survivors) else "stale: names no surviving mutant"
            lines.append(f"- {mutant}: {reason} ({status})")
    else:
        lines.append("None.")
    lines += ["", "## Needs triage", ""]
    if fresh:
        for mutant in sorted(fresh, key=lambda m: m["id"]):
            lines.append(f"- {mutant['file']}:{mutant['line']}: {mutant['mutator']}")
    else:
        lines.append("None.")
    lines += [""]
    Path(args.out).write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
