"""Decide whether the budgets job posts or updates its PR comment.

The budgets gate renders the flash, RAM and stack table on every run, and the
workflow always drops that table into the job summary. A PR comment is noise
unless something moved, so this helper posts the table only when a number
differs from its base value or a budget is breached, and clears an earlier
comment once the numbers match the base again.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

MARKER = "<!-- r2s-budgets -->"
CLEARED = (
    "### Flash, RAM and stack\n\nThe numbers match the base build again, so the earlier table no longer applies.\n"
)


class BudgetsCommentError(Exception):
    """A decision input cannot be used as given."""


def _read_mapping(path):
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise BudgetsCommentError(f"{path} cannot be read: {error}") from error
    if not isinstance(data, dict):
        raise BudgetsCommentError(f"{path} holds no mapping")
    return data


def _read_budgets(path):
    from size_report import load_budgets

    try:
        return load_budgets(Path(path))
    except Exception as error:
        raise BudgetsCommentError(f"budgets file {path} cannot be used: {error}") from error


def numbers_changed(head_sizes, base_sizes, head_stacks, base_stacks):
    """Whether any reported number moved: firmware came or went, or a flash, RAM or stack total differs."""
    if base_sizes is None or base_stacks is None:
        return False
    if set(head_sizes) != set(base_sizes) or set(head_stacks) != set(base_stacks):
        return True
    for name, sizes in head_sizes.items():
        base = base_sizes[name]
        if sizes.get("flash") != base.get("flash") or sizes.get("ram") != base.get("ram"):
            return True
    for name, worst in head_stacks.items():
        if worst.get("total") != base_stacks[name].get("total"):
            return True
    return False


def breached(head_sizes, head_stacks, budgets, base_budgets=None):
    """Every budget problem in the head numbers, against the head budgets and the base ones when given."""
    from size_report import check_sizes
    from stack_report import check_stacks

    problems = check_sizes(head_sizes, budgets) + check_stacks(head_stacks, budgets)
    if base_budgets is not None:
        problems += check_sizes(head_sizes, base_budgets) + check_stacks(head_stacks, base_budgets)
    return problems


def decide(
    head_sizes,
    head_stacks,
    budgets,
    table_text,
    base_sizes=None,
    base_stacks=None,
    base_budgets=None,
    has_comment=False,
):
    """Pick the comment action: the table, a clearing note, or nothing."""
    try:
        changed = numbers_changed(head_sizes, base_sizes, head_stacks, base_stacks)
        problems = breached(head_sizes, head_stacks, budgets, base_budgets)
    except Exception as error:
        raise BudgetsCommentError(f"the numbers cannot be judged: {error}") from error
    if changed or problems:
        return ("comment", f"{MARKER}\n{table_text}")
    if has_comment:
        return ("cleared", f"{MARKER}\n{CLEARED}")
    return ("skip", "")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--measurements", required=True)
    parser.add_argument("--stack", required=True)
    parser.add_argument("--budgets", required=True)
    parser.add_argument("--table", required=True)
    parser.add_argument("--base-measurements")
    parser.add_argument("--base-stack")
    parser.add_argument("--base-budgets")
    parser.add_argument("--has-comment", choices=("0", "1"), default="0")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    try:
        try:
            table_text = Path(args.table).read_text(encoding="utf-8")
        except OSError as error:
            raise BudgetsCommentError(f"{args.table} cannot be read: {error}") from error
        head_sizes = _read_mapping(args.measurements)
        head_stacks = _read_mapping(args.stack)
        budgets = _read_budgets(args.budgets)
        base_sizes = base_stacks = base_budgets = None
        if args.base_measurements and args.base_stack:
            base_sizes = _read_mapping(args.base_measurements)
            base_stacks = _read_mapping(args.base_stack)
        if args.base_budgets:
            base_budgets = _read_budgets(args.base_budgets)
        action, body = decide(
            head_sizes,
            head_stacks,
            budgets,
            table_text,
            base_sizes=base_sizes,
            base_stacks=base_stacks,
            base_budgets=base_budgets,
            has_comment=args.has_comment == "1",
        )
    except BudgetsCommentError as error:
        print(f"budgets_comment: error: {error}", file=sys.stderr)
        return 2
    if action != "skip":
        text = body if body.endswith("\n") else body + "\n"
        Path(args.out).write_text(text, encoding="utf-8")
    print(action)
    return 0


if __name__ == "__main__":
    sys.exit(main())
