import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def test_core_tests_run_in_random_order():
    ctest = [ln for ln in (ROOT / "scripts/core-test.sh").read_text().splitlines() if re.match(r"\s*ctest ", ln)]
    assert ctest and all("--schedule-random" in ln for ln in ctest), ctest


def test_every_unit_test_function_is_its_own_ctest_entry():
    text = (ROOT / "core/tests/CMakeLists.txt").read_text()
    assert "unity_runner.py" in text and " names " in text
