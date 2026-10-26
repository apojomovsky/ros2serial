"""Head-vs-base coverage gate: fail a PR that lowers a component's figure.

Reads gcovr --json-summary files, compares the head figure against the base
figure and the absolute floors in docs/01-testing.md, section 6. A drop
against base always fails; a floor breach fails unless the component's floor
is passed as advisory, in which case it is reported without failing.
"""

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

FLOORS = {"core": (95.0, 90.0), "bridge": (90.0, 80.0)}


@dataclass
class Figure:
    line: float
    branch: float
    lines: int = 0
    branches: int = 0


def _percent(covered: float, total: float) -> float:
    return 0.0 if total == 0 else 100.0 * covered / total


def read_summary(path: str) -> Figure:
    payload = json.loads(Path(path).read_text())
    return Figure(
        line=_percent(payload["line_covered"], payload["line_total"]),
        branch=_percent(payload["branch_covered"], payload["branch_total"]),
        lines=payload["line_total"],
        branches=payload["branch_total"],
    )


def check(
    component: str, head: Figure, base: Figure | None, advisory_floors: set[str] = frozenset()
) -> tuple[list[str], list[str]]:
    if component not in FLOORS:
        return [f"coverage: unknown component {component}"], []
    if head.lines == 0 and head.branches == 0:
        return [f"coverage: {component} measured no lines"], []
    if base is not None and base.lines == 0 and base.branches == 0:
        return [f"coverage: {component} measured no lines on base"], []
    errors, advisories = [], []
    floor_line, floor_branch = FLOORS[component]
    bucket = advisories if component in advisory_floors else errors
    if head.line < floor_line:
        bucket.append(f"coverage: {component} line {head.line:.2f}% is below the floor {floor_line:.0f}%")
    if head.branch < floor_branch:
        bucket.append(f"coverage: {component} branch {head.branch:.2f}% is below the floor {floor_branch:.0f}%")
    if base is not None:
        if head.line < base.line:
            errors.append(f"coverage: {component} line {head.line:.2f}% is below base {base.line:.2f}%")
        if head.branch < base.branch:
            errors.append(f"coverage: {component} branch {head.branch:.2f}% is below base {base.branch:.2f}%")
    return errors, advisories


def check_files(
    component: str, head: str, base: str | None, advisory_floors: set[str] = frozenset()
) -> tuple[list[str], list[str]]:
    head_figure = read_summary(head)
    base_figure = read_summary(base) if base is not None else None
    return check(component, head_figure, base_figure, advisory_floors)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--base")
    parser.add_argument("--advisory-floor", action="append", default=[])
    args = parser.parse_args(argv)
    head_figure = read_summary(args.head)
    errors, advisories = check_files(args.component, args.head, args.base, set(args.advisory_floor))
    for line in advisories:
        print(f"{line} (advisory)")
    if errors:
        print("\n".join(errors))
        return 1
    print(f"coverage: {args.component} line {head_figure.line:.2f}% branch {head_figure.branch:.2f}%: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
