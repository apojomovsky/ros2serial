"""One e2e section next to another on one host: both pass without cross-talk.

Each run.sh and run-avr.sh leases its own DDS domain and zenoh port, so two
suites never see each other's topics even though they share the tree.
Narrowed to Fast DDS and Zenoh, one board for the avr side; the RMW matrix
covers each leg alone, and these tests cover the overlap.
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
    match = re.search(r"e2e isolation: ROS_DOMAIN_ID=(\d+)", output)
    assert match is not None, "no leased domain in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


def port_of(output):
    """The leased zenoh port the run reports, failing the run that reports none."""
    match = re.search(r"ZENOH_ROUTER_PORT=(\d+)", output)
    assert match is not None, "no leased zenoh port in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_two_concurrent_e2e_sections_pass_without_crosstalk():
    env = dict(os.environ, R2S_RMWS="rmw_fastrtps_cpp rmw_zenoh_cpp")
    first = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    second = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    out1, _ = first.communicate()
    out2, _ = second.communicate()
    assert first.returncode == 0, "first section failed:\n" + out1[-4000:]
    assert second.returncode == 0, "second section failed:\n" + out2[-4000:]
    assert domain_of(out1) != domain_of(out2), "both sections ran on one domain"


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_avr_and_posix_sections_pass_side_by_side():
    env = dict(os.environ, R2S_RMWS="rmw_fastrtps_cpp rmw_zenoh_cpp", R2S_E2E_BOARDS="uno")
    avr = subprocess.Popen(
        ["bash", "tests/e2e/run-avr.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    posix = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    out_avr, _ = avr.communicate()
    out_posix, _ = posix.communicate()
    assert avr.returncode == 0, "avr section failed:\n" + out_avr[-4000:]
    assert posix.returncode == 0, "posix section failed:\n" + out_posix[-4000:]
    assert domain_of(out_avr) != domain_of(out_posix), "both sections ran on one domain"
    assert port_of(out_avr) != port_of(out_posix), "both sections share a zenoh port"
