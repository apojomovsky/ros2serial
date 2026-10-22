import subprocess
import sys
from pathlib import Path

import commit_check
import pytest

GOOD = [
    "feat(core): add the rCOBS encoder",
    "fix(bridge): drop frames longer than max_frame",
    "docs: keep master as the default branch",
    "feat(spec)!: widen topic IDs to 16 bits",
    "chore(ci): add repository skeleton\n\nWith the layout check from the first commit.",
]


@pytest.mark.parametrize("message", GOOD)
def test_good_messages_pass(message):
    assert commit_check.check_message(message) == []


@pytest.mark.parametrize(
    ("message", "problem"),
    [
        ("add the rCOBS encoder", "subject must be"),
        ("feature(core): add the encoder", "subject must be"),
        ("feat(Core): add the encoder", "subject must be"),
        ("feat(core):add the encoder", "subject must be"),
        ("feat(core): add the encoder \u2014 streaming", "em-dash"),
        ("feat(core): a\n\nline two\nline three\nline four", "at most 3 lines"),
        ("feat(core): a\n\nCo-Authored-By: Someone <s@example.com>", "trailer"),
        ("feat(core): a\n\nSigned-off-by: Someone <s@example.com>", "trailer"),
        ("feat(spec): widen IDs\n\nBREAKING CHANGE: IDs are 16 bits", "mark breaking changes with !"),
    ],
)
def test_bad_messages_are_rejected_with_a_reason(message, problem):
    problems = commit_check.check_message(message)
    assert any(problem in p for p in problems), problems


def test_git_comment_lines_and_trailing_blanks_do_not_count():
    message = "fix(core): a\n\n# Please enter the commit message\n# Lines starting with '#' are ignored\n\n\n"
    assert commit_check.check_message(message) == []


def test_title_must_be_a_single_line():
    assert any("single line" in p for p in commit_check.check_title("fix(core): a\nb"))
    assert commit_check.check_title("fix(core): a") == []


def test_a_title_problem_is_printed_once_however_many_commits_cause_it(tmp_path):
    def git(*args):
        subprocess.run(["git", *args], cwd=tmp_path, check=True, capture_output=True)

    git("init", "-q", "-b", "master")
    git("config", "user.email", "t@example.com")
    git("config", "user.name", "t")
    git("commit", "-q", "--allow-empty", "-m", "chore: start")
    for n in range(3):
        git("commit", "-q", "--allow-empty", "-m", f"fix(core): thing {n}")
    script = Path(commit_check.__file__)
    out = subprocess.run(
        [sys.executable, str(script), "--title", "docs(core): things", "--covers-range", "HEAD~3..HEAD"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    lines = out.stdout.splitlines()
    assert out.returncode == 1
    assert len(lines) == len(set(lines)) == 1, lines
