from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def test_core_tests_run_in_random_order():
    assert "--schedule-random" in (ROOT / "scripts/core-test.sh").read_text()
