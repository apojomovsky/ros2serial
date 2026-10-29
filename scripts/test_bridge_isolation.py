"""Two bridge sections side by side on one host: both pass without cross-talk.

Each bridge-test.sh run leases its own DDS domain from the same slot pool
as the e2e runners, so two suites never see each other's nodes even though
they share the tree. The shared bridge workspace stays serialized by its
build lock while each run holds its own lease.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def domain_of(output):
    """The leased domain the run reports, failing the run that reports none."""
    match = re.search(r"bridge isolation: ROS_DOMAIN_ID=(\d+)", output)
    assert match is not None, "no leased domain in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_two_concurrent_bridge_sections_pass_without_crosstalk():
    first = subprocess.Popen(
        ["bash", "scripts/bridge-test.sh"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=dict(os.environ),
    )
    second = subprocess.Popen(
        ["bash", "scripts/bridge-test.sh"],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=dict(os.environ),
    )
    out1, _ = first.communicate()
    out2, _ = second.communicate()
    assert first.returncode == 0, "first section failed:\n" + out1[-4000:]
    assert second.returncode == 0, "second section failed:\n" + out2[-4000:]
    assert domain_of(out1) != domain_of(out2), "both sections ran on one domain"


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_a_lone_bridge_run_keeps_domain_zero(tmp_path):
    env = dict(os.environ, R2S_CACHE_DIR=str(tmp_path))
    result = subprocess.run(
        ["bash", "scripts/bridge-test.sh", "--build-only"], cwd=ROOT, capture_output=True, text=True, env=env
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert domain_of(result.stdout) == 0, "a lone bridge run left domain 0:\n" + result.stdout[-4000:]
