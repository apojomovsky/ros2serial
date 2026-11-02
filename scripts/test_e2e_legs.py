"""The posix runner honors R2S_E2E_LEGS, and the isolation pairs pin one leg.

Pairs run the static leg only, so every RMW gets a single rendezvous'd
pass on both sides; standalone runs keep both legs. These tests run the
runner's own loop header with stubbed bodies, never a suite.
"""

import shlex
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
RUN_SH = ROOT / "tests" / "e2e" / "run.sh"
ISOLATION = ROOT / "scripts" / "test_e2e_isolation.py"

LEG_HEADER = "for leg in ${R2S_E2E_LEGS:-static runtime}; do"


def loop_header():
    """The leg loop header exactly as the runner carries it."""
    matches = [line for line in RUN_SH.read_text().splitlines() if line.strip() == LEG_HEADER]
    assert len(matches) == 1, "posix loop header changed shape"
    return matches[0]


def legs_of(assignments):
    """The legs the runner's loop header expands to for the given variables."""
    script = loop_header() + '\n  echo "LEG $leg"\ndone\n'
    setup = "".join(f"{name}={shlex.quote(value)}\n" for name, value in assignments.items())
    proc = subprocess.run(["bash", "-c", setup + script], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return [line.split(" ", 1)[1] for line in proc.stdout.splitlines() if line.startswith("LEG ")]


def test_pair_runs_the_static_leg_only():
    assert legs_of({"R2S_E2E_LEGS": "static"}) == ["static"]


def test_standalone_runs_both_legs():
    assert legs_of({}) == ["static", "runtime"]


def test_loop_header_still_names_both_legs():
    """The pair narrows the loop; deleting the runtime leg here would dodge it everywhere."""
    assert LEG_HEADER in RUN_SH.read_text()


def test_pairs_pin_the_static_leg():
    """run_pair tells both runners to run the static leg only."""
    assert 'R2S_E2E_LEGS="static"' in ISOLATION.read_text()
