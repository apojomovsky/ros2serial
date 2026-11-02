"""The budgets comment decision posts only when a number moves or a budget breaks.

The table reaches the job summary on every run, while the PR comment appears
only when a number differs from its base value or a budget is breached. An
earlier comment is edited instead of duplicated, and a return to the base
numbers clears the comment instead of leaving a stale table.
"""

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import budgets_comment

SCRIPT = Path(__file__).resolve().parent / "budgets_comment.py"
FIRMWARE = "arduino_minimal/uno/arduino_minimal.ino.elf"


def sizes(flash=8000, ram=300):
    return {FIRMWARE: {"flash": flash, "ram": ram}}


def stacks(total=120, unknown=()):
    worst = {"total": total, "path": ["main"], "unknown": list(unknown), "dynamic": [], "cycles": []}
    return {FIRMWARE: worst}


def limits(flash=9000, ram=1000, stack=200):
    return {"firmware": {FIRMWARE: {"flash": flash, "ram": ram, "stack": stack}}}


def decide(
    head_sizes,
    head_stacks,
    base_sizes=None,
    base_stacks=None,
    over=None,
    base_over=None,
    table="TABLE\n",
    marked=False,
):
    return budgets_comment.decide(
        head_sizes,
        head_stacks,
        over if over is not None else limits(),
        table,
        base_sizes=base_sizes,
        base_stacks=base_stacks,
        base_budgets=base_over,
        has_comment=marked,
    )


def test_matching_numbers_without_prior_comment_skip():
    action, body = decide(sizes(), stacks(), base_sizes=sizes(), base_stacks=stacks())

    assert action == "skip"
    assert body == ""


def test_flash_move_comments_the_table():
    action, body = decide(sizes(flash=8001), stacks(), base_sizes=sizes(), base_stacks=stacks())

    assert action == "comment"
    assert body.splitlines()[0] == budgets_comment.MARKER
    assert "TABLE" in body


def test_ram_move_comments_the_table():
    action, body = decide(sizes(ram=301), stacks(), base_sizes=sizes(), base_stacks=stacks())

    assert action == "comment"
    assert "TABLE" in body


def test_stack_move_comments_the_table():
    action, body = decide(sizes(), stacks(total=121), base_sizes=sizes(), base_stacks=stacks())

    assert action == "comment"
    assert "TABLE" in body


def test_added_firmware_comments_the_table():
    action, body = decide(sizes(), stacks(), base_sizes={}, base_stacks={})

    assert action == "comment"
    assert "TABLE" in body


def test_removed_firmware_comments_the_table():
    action, body = decide({}, {}, base_sizes=sizes(), base_stacks=stacks())

    assert action == "comment"
    assert "TABLE" in body


def test_breach_with_unchanged_numbers_comments_the_table():
    tight = limits(flash=7000)

    action, body = decide(sizes(), stacks(), base_sizes=sizes(), base_stacks=stacks(), over=tight)

    assert action == "comment"
    assert "TABLE" in body


def test_base_budget_breach_with_unchanged_numbers_comments_the_table():
    action, body = decide(
        sizes(),
        stacks(),
        base_sizes=sizes(),
        base_stacks=stacks(),
        over=limits(flash=99999, ram=99999, stack=99999),
        base_over=limits(flash=7000),
    )

    assert action == "comment"
    assert "TABLE" in body


def test_changed_unknown_notes_alone_skip():
    action, body = decide(sizes(), stacks(unknown=["libc"]), base_sizes=sizes(), base_stacks=stacks())

    assert action == "skip"
    assert body == ""


def test_return_to_base_with_prior_comment_clears_it():
    action, body = decide(sizes(), stacks(), base_sizes=sizes(), base_stacks=stacks(), marked=True)

    assert action == "cleared"
    assert body.splitlines()[0] == budgets_comment.MARKER
    assert "TABLE" not in body


def test_missing_base_without_breach_skips():
    action, body = decide(sizes(), stacks())

    assert action == "skip"
    assert body == ""


def test_missing_base_with_breach_comments_the_table():
    action, body = decide(sizes(), stacks(), over=limits(flash=7000))

    assert action == "comment"
    assert "TABLE" in body


def write_inputs(root: Path, head_sizes, head_stacks, over, base_sizes=None, base_stacks=None, table="TABLE\n"):
    (root / "head_sizes.json").write_text(json.dumps(head_sizes), encoding="utf-8")
    (root / "head_stacks.json").write_text(json.dumps(head_stacks), encoding="utf-8")
    (root / "budgets.yaml").write_text(
        "firmware:\n"
        f"  {FIRMWARE}:\n"
        f"    flash: {over['firmware'][FIRMWARE]['flash']}\n"
        f"    ram: {over['firmware'][FIRMWARE]['ram']}\n"
        f"    stack: {over['firmware'][FIRMWARE]['stack']}\n",
        encoding="utf-8",
    )
    (root / "table.md").write_text(table, encoding="utf-8")
    args = [
        sys.executable,
        str(SCRIPT),
        "--measurements",
        str(root / "head_sizes.json"),
        "--stack",
        str(root / "head_stacks.json"),
        "--budgets",
        str(root / "budgets.yaml"),
        "--table",
        str(root / "table.md"),
        "--out",
        str(root / "body.md"),
    ]
    if base_sizes is not None and base_stacks is not None:
        (root / "base_sizes.json").write_text(json.dumps(base_sizes), encoding="utf-8")
        (root / "base_stacks.json").write_text(json.dumps(base_stacks), encoding="utf-8")
        args += ["--base-measurements", str(root / "base_sizes.json"), "--base-stack", str(root / "base_stacks.json")]
    return args


def test_cli_writes_the_body_and_prints_comment(tmp_path):
    args = write_inputs(tmp_path, sizes(flash=8001), stacks(), limits(), sizes(), stacks())

    result = subprocess.run(args, capture_output=True, text=True, check=False)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "comment"
    body = (tmp_path / "body.md").read_text(encoding="utf-8")
    assert body.splitlines()[0] == budgets_comment.MARKER
    assert "TABLE" in body


def test_cli_reports_skip_without_writing(tmp_path):
    args = write_inputs(tmp_path, sizes(), stacks(), limits(), sizes(), stacks())

    result = subprocess.run(args, capture_output=True, text=True, check=False)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "skip"
    assert not (tmp_path / "body.md").exists()


def test_cli_reports_cleared_for_a_return_with_prior_comment(tmp_path):
    args = write_inputs(tmp_path, sizes(), stacks(), limits(), sizes(), stacks()) + ["--has-comment", "1"]

    result = subprocess.run(args, capture_output=True, text=True, check=False)

    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "cleared"
    body = (tmp_path / "body.md").read_text(encoding="utf-8")
    assert body.splitlines()[0] == budgets_comment.MARKER
    assert "TABLE" not in body


def test_cli_fails_on_a_missing_table(tmp_path):
    args = write_inputs(tmp_path, sizes(), stacks(), limits(), sizes(), stacks())
    table = args[args.index("--table") + 1]
    Path(table).unlink()

    result = subprocess.run(args, capture_output=True, text=True, check=False)

    assert result.returncode != 0
    assert not (tmp_path / "body.md").exists()
