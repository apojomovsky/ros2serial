"""Two e2e sections side by side on one host: both pass without crosstalk.

Each run.sh and run-avr.sh leases its own DDS domain and zenoh port, so
two suites never see each other's topics though they share the tree. The
pairs skip load-sensitive scenario 8; the standalone legs keep judging
it. The nightly workflow runs this file; PR CI does not.
"""

import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def domain_of(output):
    """The leased domain the run reports, failing the run that reports none."""
    match = re.search(r"e2e isolation: ROS_DOMAIN_ID=(\d+)", output)
    assert match is not None, "no leased domain in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


def port_of(output):
    """The leased zenoh port the run reports, failing the run that reports none."""
    match = re.search(r"e2e isolation: ROS_DOMAIN_ID=\d+ ZENOH_ROUTER_PORT=(\d+)", output)
    assert match is not None, "no leased zenoh port in the last 4000 bytes:\n" + output[-4000:]
    return int(match.group(1))


def failure_lines(output):
    """FAIL: and ERROR: lines, naming every test the section lost."""
    return [line for line in output.splitlines() if re.match(r"(FAIL|ERROR): ", line)]


class TestSectionReport(unittest.TestCase):
    def test_names_a_failure_outside_the_tail(self):
        failure = "e2e-failure: test_scenario_9_posix leg=static rmw=rmw_fastrtps_cpp exit=1"
        output = failure + "\n" + "test_x ... ok\n" * 2000
        self.assertIn("test_scenario_9_posix", section_report(output))

    def test_names_a_timeout_kill_outside_the_tail(self):
        failure = "e2e-failure: test_scenario_10_posix leg=runtime rmw=rmw_zenoh_cpp exit=124 (timeout)"
        output = failure + "\n" + "test_x ... ok\n" * 2000
        report = section_report(output)
        self.assertIn("test_scenario_10_posix", report)
        self.assertIn("timeout", report)


class TestFailureLines(unittest.TestCase):
    def test_names_each_failed_test(self):
        output = "test_a ... ok\nFAIL: test_b (mod.TestB)\nERROR: test_c (mod.TestC)\n"
        self.assertEqual(failure_lines(output), ["FAIL: test_b (mod.TestB)", "ERROR: test_c (mod.TestC)"])

    def test_ignores_passing_sections(self):
        self.assertEqual(failure_lines("test_a ... ok\nOK\n"), [])

    def test_ignores_traceback_body(self):
        output = 'FAIL: test_b (mod.TestB)\nTraceback:\n  File "x.py", line 1\nAssertionError\n'
        self.assertEqual(failure_lines(output), ["FAIL: test_b (mod.TestB)"])


def pair_build_dir(suffix):
    """One leg's R2S_BUILD_DIR: separate dirs lock separately, so the legs overlap."""
    base = os.environ.get("R2S_BUILD_DIR", "")
    if not base:
        # Mirrors the run.sh default; run.sh appends e2e/posix and locks build/.lock.
        base = os.path.join(os.path.expanduser("~"), ".cache", "ros2serial", "build" + str(ROOT).replace("/", "-"))
    return base + "-iso-" + suffix


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_two_concurrent_e2e_sections_pass_without_crosstalk():
    env_a = dict(
        os.environ,
        R2S_RMWS="rmw_fastrtps_cpp rmw_zenoh_cpp",
        R2S_E2E_SKIP="test_scenario_8_posix",
        R2S_BUILD_DIR=pair_build_dir("a"),
    )
    env_b = dict(
        os.environ,
        R2S_RMWS="rmw_fastrtps_cpp rmw_zenoh_cpp",
        R2S_E2E_SKIP="test_scenario_8_posix",
        R2S_BUILD_DIR=pair_build_dir("b"),
    )
    first = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env_a
    )
    second = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env_b
    )
    out1, _ = first.communicate()
    out2, _ = second.communicate()
    assert first.returncode == 0, "first section failed:\n" + "\n".join(failure_lines(out1) + [out1[-4000:]])
    assert second.returncode == 0, "second section failed:\n" + "\n".join(failure_lines(out2) + [out2[-4000:]])
    assert domain_of(out1) != domain_of(out2), "both sections ran on one domain"


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_avr_and_posix_sections_pass_side_by_side():
    env = dict(
        os.environ,
        R2S_RMWS="rmw_fastrtps_cpp rmw_zenoh_cpp",
        R2S_E2E_BOARDS="uno",
        R2S_E2E_SKIP="test_scenario_8_posix",
    )
    avr = subprocess.Popen(
        ["bash", "tests/e2e/run-avr.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    posix = subprocess.Popen(
        ["bash", "tests/e2e/run.sh"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env
    )
    out_avr, _ = avr.communicate()
    out_posix, _ = posix.communicate()
    assert avr.returncode == 0, "avr section failed:\n" + "\n".join(failure_lines(out_avr) + [out_avr[-4000:]])
    assert posix.returncode == 0, "posix section failed:\n" + "\n".join(failure_lines(out_posix) + [out_posix[-4000:]])
    assert domain_of(out_avr) != domain_of(out_posix), "both sections ran on one domain"
    assert port_of(out_avr) != port_of(out_posix), "both sections share a zenoh port"
