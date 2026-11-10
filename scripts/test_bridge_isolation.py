# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Two bridge sections side by side on one host: both pass without cross-talk.

Each bridge-test.sh run leases its own DDS domain from the same slot pool
as the e2e runners, so two suites never see each other's nodes even though
they share the tree. Each leg builds in its own directory, so the legs
overlap instead of queueing on one workspace lock, and each leg is bounded:
a leg past its timeout is killed and fails with its own output.
"""

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent

LEG_TIMEOUT_SECONDS = 600


@dataclass
class LegOutcome:
    """One finished leg: a timed out leg keeps the output it printed."""

    name: str
    returncode: int
    output: str
    timed_out: bool


def pair_build_dir(suffix):
    """One leg's R2S_BUILD_DIR: separate dirs lock separately, so the legs overlap."""
    base = os.environ.get('R2S_BUILD_DIR', '')
    if not base:
        # Mirrors the bridge-test.sh default; bridge-test.sh locks workspace/.lock.
        base = os.path.join(
            os.path.expanduser('~'), '.cache', 'ros2serial', 'build' + str(ROOT).replace('/', '-')
        )
    return base + '-bridge-iso-' + suffix


def start_leg(name, command, env):
    """Start one leg with its pipes held, so both legs run before either drains."""
    return subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )


def finish_leg(name, proc, timeout=LEG_TIMEOUT_SECONDS):
    """Drain one leg: a leg past its timeout is killed and keeps its output."""
    try:
        output, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate()
        return LegOutcome(name, proc.returncode, output, True)
    return LegOutcome(name, proc.returncode, output, False)


def run_leg(name, command, env=None, timeout=LEG_TIMEOUT_SECONDS):
    """Start and drain one leg: the sequential case of the pair above."""
    return finish_leg(name, start_leg(name, command, env or dict(os.environ)), timeout)


def domain_of(output):
    """The leased domain the run reports, failing the run that reports none."""
    match = re.search(r'bridge isolation: ROS_DOMAIN_ID=(\d+)', output)
    assert match is not None, 'no leased domain in the last 4000 bytes:\n' + output[-4000:]
    return int(match.group(1))


@pytest.mark.skipif(shutil.which('colcon') is None, reason='colcon is not installed')
def test_two_concurrent_bridge_sections_pass_without_crosstalk():
    env_a = dict(os.environ, R2S_BUILD_DIR=pair_build_dir('a'))
    env_b = dict(os.environ, R2S_BUILD_DIR=pair_build_dir('b'))
    first = start_leg('first', ['bash', 'scripts/bridge-test.sh'], env_a)
    second = start_leg('second', ['bash', 'scripts/bridge-test.sh'], env_b)
    out1 = finish_leg('first', first)
    out2 = finish_leg('second', second)
    for outcome in (out1, out2):
        assert not outcome.timed_out, (
            f'{outcome.name} leg timed out after {LEG_TIMEOUT_SECONDS}s:\n'
            + outcome.output[-4000:]
        )
        assert outcome.returncode == 0, (
            f'{outcome.name} section failed:\n' + outcome.output[-4000:]
        )
    assert domain_of(out1.output) != domain_of(out2.output), 'both sections ran on one domain'


@pytest.mark.skipif(shutil.which('colcon') is None, reason='colcon is not installed')
def test_a_lone_bridge_run_keeps_domain_zero(tmp_path):
    env = dict(os.environ, R2S_CACHE_DIR=str(tmp_path))
    result = subprocess.run(
        ['bash', 'scripts/bridge-test.sh', '--build-only'],
        cwd=ROOT,
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert domain_of(result.stdout) == 0, (
        'a lone bridge run left domain 0:\n' + result.stdout[-4000:]
    )
