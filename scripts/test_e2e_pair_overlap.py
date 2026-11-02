"""The posix isolation pair overlaps for real on separate build dirs.

Both legs run tests/e2e/run.sh, whose build lock is held for the whole
run: sharing one R2S_BUILD_DIR would run the legs one after the other
and prove nothing about concurrent suites. These tests load the pair's
own build dir helper and check the legs differ, without a suite.
"""

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ISOLATION = ROOT / "tests" / "e2e" / "test_isolation.py"
MODULE_NAME = "isolation_pair_probe"


def load_pair(monkeypatch, build_dir):
    """The isolation module with R2S_BUILD_DIR set or unset."""
    if build_dir is None:
        monkeypatch.delenv("R2S_BUILD_DIR", raising=False)
    else:
        monkeypatch.setenv("R2S_BUILD_DIR", build_dir)
    sys.modules.pop(MODULE_NAME, None)
    spec = importlib.util.spec_from_file_location(MODULE_NAME, ISOLATION)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def test_the_pair_builds_in_separate_directories(monkeypatch):
    pair = load_pair(monkeypatch, None)
    assert pair.pair_build_dir("a") != pair.pair_build_dir("b")


def test_an_explicit_build_dir_stays_separate(monkeypatch, tmp_path):
    pair = load_pair(monkeypatch, str(tmp_path / "build"))
    first, second = pair.pair_build_dir("a"), pair.pair_build_dir("b")
    assert first != second, "both pair legs share one build directory"
    assert str(tmp_path) in first and str(tmp_path) in second


def test_the_pairs_start_from_the_repository_root(monkeypatch):
    pair = load_pair(monkeypatch, None)
    assert (pair.ROOT / "tests" / "e2e" / "run.sh").is_file(), pair.ROOT
    assert (pair.ROOT / "tests" / "e2e" / "run-avr.sh").is_file(), pair.ROOT
