"""The e2e runners honor R2S_E2E_SKIP, and the isolation pairs skip scenario 8.

run.sh and run-avr.sh skip the test files named in R2S_E2E_SKIP, so the
concurrent pairs can drop load-sensitive scenario 8 while the standalone
legs keep judging its 1 ms bound. These tests run the runners' own skip
lines with stubbed launch commands, never a suite.
"""

import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUN_SH = ROOT / "tests" / "e2e" / "run.sh"
RUN_AVR_SH = ROOT / "tests" / "e2e" / "run-avr.sh"
ISOLATION = ROOT / "scripts" / "test_e2e_isolation.py"

ANCHOR = 'case " ${R2S_E2E_SKIP:-} " in'


def skip_block(path):
    """The R2S_E2E_SKIP case block exactly as the runner carries it."""
    lines = path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if ANCHOR in line)
    end = next(i for i in range(start, len(lines)) if lines[i].strip() == "esac")
    return "\n".join(lines[start : end + 1])


def decide(path, assignments):
    """The runner's decision for the given shell variables: skip or run."""
    prelude = 'timeout() { echo "RUN $*"; }\nlaunch_test() { echo "LAUNCH $*"; }\n'
    setup = "".join(f"{name}={shlex.quote(value)}\n" for name, value in assignments.items())
    proc = subprocess.run(["bash", "-c", prelude + setup + skip_block(path)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


@pytest.mark.parametrize(
    ("runner", "variables", "skipped"),
    [
        (RUN_SH, {"test_file": "test_scenario_8_posix", "R2S_E2E_SKIP": "test_scenario_8_posix"}, True),
        (RUN_SH, {"test_file": "test_scenario_8_posix", "R2S_E2E_SKIP": "test_scenario_9_posix"}, False),
        (RUN_SH, {"test_file": "test_scenario_8_posix"}, False),
        (
            RUN_SH,
            {"test_file": "test_scenario_8_posix", "R2S_E2E_SKIP": "test_scenario_1_posix test_scenario_8_posix"},
            True,
        ),
        (RUN_SH, {"test_file": "test_scenario_8_posix", "R2S_E2E_SKIP": "test_scenario_8_posix_extra"}, False),
        (RUN_AVR_SH, {"scenario": "8", "R2S_E2E_SKIP": "test_scenario_8_avr"}, True),
        (RUN_AVR_SH, {"scenario": "3", "R2S_E2E_SKIP": "test_scenario_8_avr"}, False),
        (RUN_AVR_SH, {"scenario": "3"}, False),
    ],
)
def test_skip_switch_decides_per_file(runner, variables, skipped):
    out = decide(runner, variables)
    if skipped:
        assert "RUN " not in out, "listed file still runs:\n" + out
    else:
        assert "RUN " in out, "unlisted file skipped:\n" + out


def test_standalone_loop_keeps_scenario_8():
    """The pairs skip scenario 8; deleting it from run.sh would dodge it everywhere."""
    assert "test_scenario_4_posix test_scenario_8_posix test_scenario_9_posix" in RUN_SH.read_text()


def test_pairs_skip_scenario_8():
    """run_pair tells both runners to sit scenario 8 out."""
    assert 'R2S_E2E_SKIP="test_scenario_8_posix"' in ISOLATION.read_text()
