"""The ros compat leg runs scenarios 1 to 4, like the static C++ leg.

Compat is a side path, so it must not cost every run a fourth full
leg. The runtime legs keep the whole file list. These tests evaluate
the runner's own leg block per leg with a stubbed launcher, never a
suite.
"""

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUN_SH = ROOT / "tests" / "e2e" / "run.sh"

ANCHOR = "case $leg in"
SHORT = "test_scenario_1_posix test_scenario_2_posix test_scenario_3_posix test_scenario_4_posix"


def leg_block():
    """The leg-to-files case block exactly as the runner carries it."""
    lines = RUN_SH.read_text().splitlines()
    starts = [i for i, line in enumerate(lines) if ANCHOR in line]
    for start in starts:
        end = next(i for i in range(start, len(lines)) if lines[i].strip() == "esac")
        if 'files="' in "\n".join(lines[start : end + 1]):
            return "\n".join(lines[start : end + 1])
    raise AssertionError("run.sh carries no leg-to-files block")


def files_for(leg):
    """The file list the runner selects for one leg."""
    script = f"leg={leg}\n" + leg_block() + '\necho "$files"\n'
    proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout.strip()


@pytest.mark.parametrize("leg", ["ros", "static_cpp"])
def test_compat_legs_cover_scenarios_1_to_4(leg):
    assert files_for(leg).split() == SHORT.split()


def test_runtime_legs_run_the_whole_list():
    full = files_for("runtime").split()
    assert full[:4] == SHORT.split()
    assert "test_scenario_6_posix" in full
