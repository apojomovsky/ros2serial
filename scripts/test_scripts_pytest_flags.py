"""The SCRIPTS pytest run names a stuck test instead of going silent.

A hung test must dump every thread's stack with the test's name
(faulthandler_timeout, built into pytest) and the run must list its
slowest tests (--durations), so the next hang bisects to a test from
the log. These tests pin both flags on the suite's pytest invocation.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def scripts_tests_body():
    """The lines of the scripts_tests function in ci-test.sh."""
    lines = (ROOT / "scripts" / "ci-test.sh").read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("scripts_tests()"))
    end = next(i for i, line in enumerate(lines) if i > start and line == "}")
    return "\n".join(lines[start:end])


def test_the_scripts_run_dumps_a_stuck_test():
    assert "faulthandler_timeout=" in scripts_tests_body()


def test_the_scripts_run_reports_its_slowest_tests():
    assert "--durations" in scripts_tests_body()
