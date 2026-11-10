# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Render the nightly fuzz seed-corpus coverage into one report.

scripts/fuzz.sh measures each target's seed corpus with gcovr and leaves
one detailed JSON summary per target; this script renders them into a
markdown report: a score row per target and the files each target's seeds
reach. A low percentage never fails: the report says what "fuzzed" means,
it gates nothing. A missing, malformed or empty summary does fail, so a
broken nightly run is loud instead of silent.
"""

import argparse
import json
import sys
from pathlib import Path


def load_summary(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except FileNotFoundError:
        sys.exit(f'fuzz_coverage: summary file not found: {path}')
    except json.JSONDecodeError as exc:
        sys.exit(f'fuzz_coverage: summary file is not valid JSON: {path}: {exc}')


def target_lines(summary, target, path):
    # The gcovr detailed format: one entry per file with one count per
    # executable line, as gcovr --json writes it.
    try:
        rows = [
            (f['file'], sum(1 for line in f['lines'] if line['count'] > 0), len(f['lines']))
            for f in summary['files']
        ]
    except (KeyError, TypeError):
        sys.exit(f'fuzz_coverage: summary has no file line counts: {path}')
    covered = sum(row[1] for row in rows)
    total = sum(row[2] for row in rows)
    if total == 0:
        sys.exit(f'fuzz_coverage: {target} measured no lines: {path}')
    return covered, total, [row[0] for row in rows]


def render(targets):
    lines = ['# Fuzz seed-corpus coverage', '']
    lines += [
        '| Target | Lines | Percent | Files |',
        '| --- | --- | --- | --- |',
    ]
    for target, covered, total, files in targets:
        percent = 100 * covered // total
        lines.append(f'| {target} | {covered}/{total} | {percent}% | {len(files)} |')
    for target, _covered, _total, files in targets:
        lines += ['', f'## {target}', '']
        for name in sorted(files):
            lines.append(f'- {name}')
    lines += ['']
    return '\n'.join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', action='append', default=[])
    parser.add_argument('--summary', action='append', default=[])
    parser.add_argument('--out', required=True)
    args = parser.parse_args(argv)
    if len(args.target) != len(args.summary) or not args.target:
        missing = next((t for t in args.target if t not in args.summary), args.target)
        sys.exit(f'fuzz_coverage: each --target needs one --summary: {missing}')
    targets = []
    for target, path in zip(args.target, args.summary, strict=True):
        summary = load_summary(path)
        covered, total, files = target_lines(summary, target, path)
        targets.append((target, covered, total, files))
    Path(args.out).write_text(render(targets), encoding='utf-8')


if __name__ == '__main__':
    main()
