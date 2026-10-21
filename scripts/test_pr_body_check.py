import pr_body_check

GOOD = "Closes #1\n\n## Failing run first\n\n```\n1 failed\n```\n\n## Review\n\nNo findings.\n"


def test_a_good_body_passes_both_modes():
    assert pr_body_check.check(GOOD, full=True) == []
    assert pr_body_check.check(GOOD, full=False) == []


def test_literal_newline_escapes_fail_in_ci_mode():
    assert any("literal" in p for p in pr_body_check.check("one\\ntwo", full=False))
    assert any("literal" in p for p in pr_body_check.check("one\\rtwo", full=False))


def test_escapes_inside_code_are_allowed():
    body = GOOD + '\nNever `--body "a\\nb"`.\n\n```\nprintf("x\\n");\n```\n'
    assert pr_body_check.check(body, full=True) == []


def test_ci_mode_does_not_require_the_ritual_sections():
    assert pr_body_check.check("A maintainer's quick fix.", full=False) == []


def test_full_mode_requires_closes_review_and_the_failing_run():
    problems = pr_body_check.check("Just text.", full=True)
    assert any("Closes #N" in p for p in problems)
    assert any("## Review" in p for p in problems)
    assert any("failing test run" in p for p in problems)


def test_tilde_fences_and_indented_code_are_code():
    body = GOOD + "\n~~~\necho a\\nb\n~~~\n\n    printf('x\\n')\n"
    assert pr_body_check.check(body, full=True) == []


def test_closes_is_case_insensitive():
    assert pr_body_check.check(GOOD.replace("Closes", "closes"), full=True) == []
