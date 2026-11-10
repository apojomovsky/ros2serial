#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Merge normalized mutation results into one triage report.

Mull and mutmut speak different formats, so scripts/run-mutation.sh
normalizes each into {"component", "mutants": [{id, file, line, mutator,
outcome}]} before calling this. This script scores each component against
the floors in tools/robustness.yaml (which repeat docs/01-testing.md,
section 6), lists the surviving mutants that still need a test, and
records the acknowledged equivalents with their reasons. Without --gate
a low score only shows in the report; with it a measured score below its
floor fails, so the nightly run goes red. Malformed input always fails,
so a broken nightly run is loud.
"""

import argparse
import json
import sys
from pathlib import Path

import yaml

DEFAULT_FLOORS = Path(__file__).resolve().parent.parent / 'tools' / 'robustness.yaml'

# Every area the report scores, whether the nightly run measures it yet or
# not: an unmeasured area reads "no data" and never fails the gate.
COMPONENTS = ('core', 'cpp', 'gen', 'bridge')


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except FileNotFoundError:
        sys.exit(f'mutation_triage: result file not found: {path}')
    except json.JSONDecodeError as exc:
        sys.exit(f'mutation_triage: result file is not valid JSON: {path}: {exc}')


def load_equivalent(path):
    try:
        entries = yaml.safe_load(Path(path).read_text(encoding='utf-8')) or []
    except FileNotFoundError:
        sys.exit(f'mutation_triage: equivalent file not found: {path}')
    known = {}
    for entry in entries:
        known[entry['mutant']] = entry['reason']
    return known


def load_floors(path):
    try:
        floors = yaml.safe_load(Path(path).read_text(encoding='utf-8'))['mutation_floors']
    except FileNotFoundError:
        sys.exit(f'mutation_triage: floors file not found: {path}')
    except (yaml.YAMLError, KeyError, TypeError) as exc:
        sys.exit(f'mutation_triage: floors file is not valid: {path}: {exc}')
    for component in COMPONENTS:
        if component not in floors:
            sys.exit(f'mutation_triage: floors file names no floor for {component}: {path}')
    return floors


def check_results(results, floors):
    for result in results:
        if result.get('component') not in floors:
            sys.exit(f'mutation_triage: unknown component: {result.get("component")!r}')
        for mutant in result.get('mutants', []):
            if mutant.get('outcome') not in ('survived', 'killed'):
                sys.exit(f'mutation_triage: unknown outcome for {mutant.get("id")!r}')


def score(mutants):
    killed = sum(1 for mutant in mutants if mutant['outcome'] == 'killed')
    total = len(mutants)
    percent = 'n/a' if total == 0 else f'{100 * killed // total}%'
    return killed, total, percent


def gate_failures(by_component, floors):
    failures = []
    for component in COMPONENTS:
        killed, total, percent = score(by_component.get(component, []))
        if total and 100 * killed // total < floors[component]:
            failures.append(
                f'mutation: {component} {percent} is below the floor {floors[component]}%'
            )
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mull', required=True)
    parser.add_argument('--mutmut', required=True)
    parser.add_argument('--equivalent', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--gate', action='store_true')
    parser.add_argument('--floors', default=str(DEFAULT_FLOORS))
    args = parser.parse_args()
    floors = load_floors(args.floors)
    results = [load_json(args.mull), load_json(args.mutmut)]
    check_results(results, floors)
    known = load_equivalent(args.equivalent)
    by_component = {result['component']: result.get('mutants', []) for result in results}
    lines = ['# Mutation triage', '']
    lines += [
        '| Component | Killed | Total | Score | Floor | Status |',
        '| --- | --- | --- | --- | --- | --- |',
    ]
    for component in COMPONENTS:
        killed, total, percent = score(by_component.get(component, []))
        mark = (
            'ok'
            if total and 100 * killed // total >= floors[component]
            else ('below floor' if total else 'no data')
        )
        lines.append(
            f'| {component} | {killed} | {total} | {percent} | {floors[component]}% | {mark} |'
        )
    survivors = [
        m for mutants in by_component.values() for m in mutants if m['outcome'] == 'survived'
    ]
    fresh = [m for m in survivors if m['id'] not in known]
    lines += ['', '## Acknowledged equivalents', '']
    if known:
        for mutant, reason in sorted(known.items()):
            status = (
                'seen'
                if any(m['id'] == mutant for m in survivors)
                else 'stale: names no surviving mutant'
            )
            lines.append(f'- {mutant}: {reason} ({status})')
    else:
        lines.append('None.')
    lines += ['', '## Needs triage', '']
    if fresh:
        for mutant in sorted(fresh, key=lambda m: m['id']):
            lines.append(f'- {mutant["file"]}:{mutant["line"]}: {mutant["mutator"]}')
    else:
        lines.append('None.')
    lines += ['']
    Path(args.out).write_text('\n'.join(lines), encoding='utf-8')
    if args.gate:
        failures = gate_failures(by_component, floors)
        if failures:
            sys.exit('\n'.join(failures))


if __name__ == '__main__':
    main()
