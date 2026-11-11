# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Coverage gate: judge the lines a pull request changes, plus the floors.

Reads a gcovr --json detail file (core, bridge) or the coverage.py JSON
report (gen), the diff between the merge base and HEAD, and the head sources.
A changed measured line fails the gate when it is uncovered, or when it is
excluded without an in-source exclusion marker carrying a reason. A drop of
the whole-package figure against base no longer fails: the patch figure
replaces it. The base measurement stays: the job measures it with the same
command and reports it next to the head figure. The absolute line and branch
floors in docs/01-testing.md, section 6, still fail the gate, so a change
cannot land on a tree that is already below the floor.
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass
from dataclasses import field
from pathlib import Path

FLOORS = {'core': (95.0, 90.0), 'bridge': (90.0, 80.0), 'gen': (95.0, 90.0)}


@dataclass
class Figure:
    line: float
    branch: float
    lines: int = 0
    branches: int = 0


@dataclass
class PatchFigure:
    lines_changed: int = 0
    lines_covered: int = 0
    lines_excluded: int = 0
    branches_changed: int = 0
    branches_covered: int = 0
    branches_excluded: int = 0


@dataclass
class LineCov:
    count: int
    excluded: bool
    branches: list = field(default_factory=list)


def _percent(covered: float, total: float) -> float:
    return 0.0 if total == 0 else 100.0 * covered / total


def read_summary(path: str) -> Figure:
    payload = json.loads(Path(path).read_text())
    return Figure(
        line=_percent(payload['line_covered'], payload['line_total']),
        branch=_percent(payload['branch_covered'], payload['branch_total']),
        lines=payload['line_total'],
        branches=payload['branch_total'],
    )


def read_totals(component: str, path: str) -> Figure:
    if component == 'gen':
        totals = json.loads(Path(path).read_text())['totals']
        return Figure(
            line=_percent(totals['covered_lines'], totals['num_statements']),
            branch=_percent(totals['covered_branches'], totals['num_branches']),
            lines=totals['num_statements'],
            branches=totals['num_branches'],
        )
    return read_summary(path)


def _gcovr_branches(raw: list) -> list:
    return [
        {
            'count': branch.get('count', 0),
            'excluded': bool(branch.get('gcovr/excluded', False)),
            'throw': bool(branch.get('throw', False)),
        }
        for branch in raw
    ]


def read_detail_gcovr(path: str) -> dict:
    detail = {}
    for entry in json.loads(Path(path).read_text())['files']:
        lines = {}
        for line in entry.get('lines', []):
            lines[line['line_number']] = LineCov(
                count=line.get('count', 0),
                excluded=bool(line.get('gcovr/excluded', False)),
                branches=_gcovr_branches(line.get('branches', [])),
            )
        detail[entry['file']] = lines
    return detail


def read_detail_coveragepy(path: str) -> dict:
    detail = {}
    for rel, entry in json.loads(Path(path).read_text())['files'].items():
        lines = {}
        for number in entry.get('executed_lines', []):
            lines[number] = LineCov(count=1, excluded=False)
        for number in entry.get('missing_lines', []):
            lines[number] = LineCov(count=0, excluded=False)
        for number in entry.get('excluded_lines', []):
            lines[number] = LineCov(count=0, excluded=True)
        arcs: dict = {}
        for start, _ in entry.get('executed_branches', []):
            arcs.setdefault(start, []).append({'count': 1, 'excluded': False, 'throw': False})
        for start, _ in entry.get('missing_branches', []):
            arcs.setdefault(start, []).append({'count': 0, 'excluded': False, 'throw': False})
        for start, branches in arcs.items():
            if start in lines:
                lines[start].branches = branches
        detail[rel] = lines
    return detail


def read_detail(component: str, path: str) -> dict:
    if component == 'gen':
        return read_detail_coveragepy(path)
    if component in FLOORS:
        return read_detail_gcovr(path)
    raise ValueError(f'coverage: unknown component {component}')


_HUNK_RE = re.compile(r'@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@')


def parse_diff(text: str) -> dict:
    changed: dict = {}
    path = None
    number = 0
    for raw in text.splitlines():
        if raw.startswith('+++ '):
            name = raw[4:].strip()
            if name in ('/dev/null', '"/dev/null"'):
                path = None
                continue
            if name.startswith('b/'):
                name = name[2:]
            if len(name) > 1 and name.startswith('"') and name.endswith('"'):
                name = name[1:-1]
            path = name
            changed.setdefault(path, [])
        elif raw.startswith('@@ '):
            match = _HUNK_RE.search(raw)
            number = int(match.group(1)) if match else 0
        elif path is None:
            continue
        elif raw.startswith('+') and not raw.startswith('+++'):
            changed[path].append(number)
            number += 1
        elif raw.startswith('-') and not raw.startswith('---'):
            continue
        else:
            number += 1
    return {name: lines for name, lines in changed.items() if lines}


_LINE_MARK_RE = re.compile(r'GCOVR?_EXCL_(?:BR_)?LINE\b(.*)$')
_START_MARK_RE = re.compile(r'GCOVR?_EXCL_(?:BR_)?START\b(.*)$')
_STOP_MARK_RE = re.compile(r'GCOVR?_EXCL_(?:BR_)?STOP\b')
_REASON_RE = re.compile(r'^\s*[:\-]\s*\S')


def gcovr_reasoned(source: list) -> set:
    reasoned = set()
    pending = []
    for number, text in enumerate(source, 1):
        start = _START_MARK_RE.search(text)
        if start:
            kind = 'BR' if 'BR_' in start.group(0) else 'LINE'
            pending.append((kind, number, bool(_REASON_RE.search(start.group(1)))))
        stop = _STOP_MARK_RE.search(text)
        if stop:
            kind = 'BR' if 'BR_' in stop.group(0) else 'LINE'
            for index in range(len(pending) - 1, -1, -1):
                if pending[index][0] == kind:
                    _, first, has_reason = pending.pop(index)
                    if has_reason:
                        reasoned.update(range(first + 1, number))
                    break
        line = _LINE_MARK_RE.search(text)
        if line and _REASON_RE.search(line.group(1)):
            reasoned.add(number)
    for _, first, has_reason in pending:
        if has_reason:
            reasoned.update(range(first + 1, len(source) + 1))
    return reasoned


_PRAGMA_RE = re.compile(r'pragma:\s*no cover(.*)$')


def coveragepy_reasoned(source: list) -> set:
    reasoned = set()
    for number, text in enumerate(source, 1):
        match = _PRAGMA_RE.search(text)
        if match and _REASON_RE.search(match.group(1)):
            reasoned.add(number)
    return reasoned


def _reasoned_for(component: str, source: list) -> set:
    if component == 'gen':
        return coveragepy_reasoned(source)
    return gcovr_reasoned(source)


def check_patch(component, detail, changed, measured, load_source) -> tuple:
    errors = []
    figure = PatchFigure()
    skipped = sorted(path for path in changed if path not in measured or path not in detail)
    for path in sorted(path for path in changed if path in measured):
        file_cov = detail.get(path)
        if file_cov is None:
            continue
        source = load_source(path)
        if source is None:
            errors.append(f'coverage: {component} patch: {path}: cannot read source')
            continue
        reasoned = _reasoned_for(component, source)
        for number in sorted(changed[path]):
            entry = file_cov.get(number)
            if entry is None:
                continue
            figure.lines_changed += 1
            if entry.excluded:
                if number in reasoned:
                    figure.lines_excluded += 1
                else:
                    errors.append(
                        f'coverage: {component} patch: {path}:{number}: excluded without a reason'
                    )
                continue
            if entry.count <= 0:
                errors.append(f'coverage: {component} patch: {path}:{number}: uncovered line')
                continue
            figure.lines_covered += 1
            problem = None
            for branch in entry.branches:
                if branch.get('throw'):
                    continue
                figure.branches_changed += 1
                if branch.get('excluded'):
                    if number in reasoned:
                        figure.branches_excluded += 1
                    elif problem is None:
                        problem = 'excluded without a reason'
                elif branch.get('count', 0) <= 0:
                    if problem is None:
                        problem = 'uncovered branch'
                else:
                    figure.branches_covered += 1
            if problem is not None:
                errors.append(f'coverage: {component} patch: {path}:{number}: {problem}')
    return errors, figure, skipped


def check(component: str, head: Figure, advisory_floors=frozenset()) -> tuple:
    if component not in FLOORS:
        return [f'coverage: unknown component {component}'], []
    if head.lines == 0 and head.branches == 0:
        return [f'coverage: {component} measured no lines'], []
    errors, advisories = [], []
    floor_line, floor_branch = FLOORS[component]
    bucket = advisories if component in advisory_floors else errors
    if head.line < floor_line:
        bucket.append(
            f'coverage: {component} line {head.line:.2f}% is below the floor {floor_line:.0f}%'
        )
    if head.branch < floor_branch:
        bucket.append(
            f'coverage: {component} branch {head.branch:.2f}% is below the floor '
            f'{floor_branch:.0f}%'
        )
    return errors, advisories


def check_files(component: str, head: str, advisory_floors=frozenset()) -> tuple:
    return check(component, read_summary(head), advisory_floors)


def _patch_report(component: str, figure: PatchFigure) -> str:
    if figure.lines_changed == 0 and figure.branches_changed == 0:
        return f'coverage: {component} patch: no measured lines changed: ok'
    return (
        f'coverage: {component} patch lines {figure.lines_covered}/{figure.lines_changed} '
        f'covered, {figure.lines_excluded} excluded; branches '
        f'{figure.branches_covered}/{figure.branches_changed} covered, '
        f'{figure.branches_excluded} excluded: ok'
    )


def gate_files(
    component: str,
    head_path: str,
    base_path=None,
    detail_path=None,
    diff_text=None,
    repo=None,
    measured=frozenset(),
    advisory_floors=frozenset(),
) -> tuple:
    if component not in FLOORS:
        return [f'coverage: unknown component {component}'], [], []
    head = read_totals(component, head_path)
    errors, advisories = check(component, head, advisory_floors)
    if errors:
        return errors, advisories, []
    summary = f'coverage: {component} line {head.line:.2f}% branch {head.branch:.2f}%'
    if detail_path is None or diff_text is None or repo is None:
        return [], advisories, [f'{summary}: ok']
    base = read_totals(component, base_path) if base_path is not None else None
    detail = read_detail(component, detail_path)
    changed = parse_diff(diff_text)
    if not measured:
        measured = set(changed)

    def load_source(path):
        target = Path(repo) / path
        if not target.is_file():
            return None
        return target.read_text().splitlines()

    patch_errors, figure, skipped = check_patch(component, detail, changed, measured, load_source)
    info = []
    if base is not None:
        info.append(f'coverage: {component} base line {base.line:.2f}% branch {base.branch:.2f}%')
    if skipped:
        lines = sum(len(changed[path]) for path in skipped)
        info.append(
            f'coverage: {component} patch: ignoring {lines} changed lines in '
            f'{len(skipped)} files with no coverage data: {" ".join(skipped)}'
        )
    if patch_errors:
        return patch_errors, advisories, info
    return [], advisories, [f'{summary}: ok'] + info + [_patch_report(component, figure)]


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', required=True)
    parser.add_argument('--head', required=True)
    parser.add_argument('--base')
    parser.add_argument('--detail')
    parser.add_argument('--diff')
    parser.add_argument('--repo')
    parser.add_argument('--measured', action='append', default=[])
    parser.add_argument('--advisory-floor', action='append', default=[])
    args = parser.parse_args(argv)
    diff_text = Path(args.diff).read_text() if args.diff else None
    errors, advisories, reports = gate_files(
        args.component,
        args.head,
        args.base,
        args.detail,
        diff_text,
        args.repo,
        set(args.measured),
        set(args.advisory_floor),
    )
    for line in advisories:
        print(f'{line} (advisory)')
    for line in reports:
        print(line)
    if errors:
        print('\n'.join(errors))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
