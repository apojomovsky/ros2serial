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
