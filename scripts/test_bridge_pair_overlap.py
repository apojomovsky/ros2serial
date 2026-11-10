# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The bridge isolation pair overlaps for real on separate build dirs.

Both legs run scripts/bridge-test.sh, whose build lock is held for the
whole run: sharing one R2S_BUILD_DIR runs the legs one after the other
behind the lock while the parent drains the first pipe, which deadlocks
when the lock holder fills its unread pipe. A stuck leg must fail with
its own output instead of hanging the suite. These tests load the pair's
own helpers, without a suite.
"""

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ISOLATION = ROOT / 'scripts' / 'test_bridge_isolation.py'
MODULE_NAME = 'bridge_isolation_pair_probe'


def load_pair(monkeypatch, build_dir):
    """The isolation module with R2S_BUILD_DIR set or unset."""
    if build_dir is None:
        monkeypatch.delenv('R2S_BUILD_DIR', raising=False)
    else:
        monkeypatch.setenv('R2S_BUILD_DIR', build_dir)
    sys.modules.pop(MODULE_NAME, None)
    spec = importlib.util.spec_from_file_location(MODULE_NAME, ISOLATION)
    module = importlib.util.module_from_spec(spec)
    sys.modules[MODULE_NAME] = module
    spec.loader.exec_module(module)
    return module


def test_the_pair_builds_in_separate_directories(monkeypatch):
    pair = load_pair(monkeypatch, None)
    assert pair.pair_build_dir('a') != pair.pair_build_dir('b')


def test_an_explicit_build_dir_stays_separate(monkeypatch, tmp_path):
    pair = load_pair(monkeypatch, str(tmp_path / 'build'))
    first, second = pair.pair_build_dir('a'), pair.pair_build_dir('b')
    assert first != second, 'both pair legs share one build directory'
    assert str(tmp_path) in first and str(tmp_path) in second


def test_a_stuck_leg_fails_with_its_output(monkeypatch, tmp_path):
    pair = load_pair(monkeypatch, str(tmp_path / 'build'))
    outcome = pair.run_leg('stuck', ['bash', '-c', 'echo marker-line; sleep 60'], timeout=5)
    assert outcome.timed_out, 'a leg past its timeout must not pass silently'
    assert 'marker-line' in outcome.output, 'a timed out leg must keep its output'
    assert outcome.returncode != 0


def test_a_quick_leg_keeps_its_output(monkeypatch, tmp_path):
    pair = load_pair(monkeypatch, str(tmp_path / 'build'))
    outcome = pair.run_leg('quick', ['bash', '-c', 'echo hello-leg'], timeout=60)
    assert not outcome.timed_out
    assert outcome.returncode == 0
    assert 'hello-leg' in outcome.output
