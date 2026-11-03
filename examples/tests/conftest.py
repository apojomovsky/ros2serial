"""Make the simavr harness helpers importable for the examples test."""

import sys
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[2] / "tests" / "harness" / "simavr"
if str(HARNESS) not in sys.path:
    sys.path.insert(0, str(HARNESS))
