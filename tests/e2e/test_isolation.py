# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Two e2e sections side by side on one host: both pass without crosstalk.

Each run.sh and run-avr.sh leases its own DDS domain and zenoh port, so
two suites never see each other's topics though they share the tree. The
pairs skip load-sensitive scenario 8; the standalone legs keep judging
it. The nightly workflow runs this file; PR CI does not.
"""

import contextlib
import io
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[2]


def domain_of(output):
    """The leased domain the run reports, failing the run that reports none."""
    match = re.search(r'e2e isolation: ROS_DOMAIN_ID=(\d+)', output)
    assert match is not None, 'no leased domain in the last 4000 bytes:\n' + output[-4000:]
    return int(match.group(1))


def port_of(output):
    """The leased zenoh port the run reports, failing the run that reports none."""
    match = re.search(r'e2e isolation: ROS_DOMAIN_ID=\d+ ZENOH_ROUTER_PORT=(\d+)', output)
    assert match is not None, 'no leased zenoh port in the last 4000 bytes:\n' + output[-4000:]
    return int(match.group(1))


def failure_lines(output):
    """Failure lines, naming every test and launch process the section lost.

    Besides the launch_test FAIL and ERROR lines, a launch whose nodes die
    before the tests complete reports no test at all: it prints that the
    processes stopped and how each exited, so those lines name the cause.
    """
    found = []
    for line in output.splitlines():
        if re.match(r'(FAIL|ERROR): ', line):
            found.append(line)
        elif line == 'Processes under test stopped before tests completed':
            found.append(line)
        elif re.match(r"Process '.+' exited with [0-9]+", line):
            found.append(line)
    return found


def summary_lines(output):
    """run.sh and run-avr.sh per-failure summary lines, naming every failed scenario file."""
    return [line for line in output.splitlines() if line.startswith('e2e-failure: ')]


def section_report(output):
    """The failure report for one isolation section.

    The summary lines name the cause whatever the output length, then the
    failure lines, then the tail for context.
    """
    return '\n'.join(summary_lines(output) + failure_lines(output) + [output[-4000:]])


def log_section_wall_time(name, start):
    """One section's wall time in the job log, so a slow pair reads as a number."""
    print(f'e2e isolation: section {name} took {time.monotonic() - start:.0f}s', flush=True)


class TestSectionReport(unittest.TestCase):
    def test_names_a_failure_outside_the_tail(self):
        failure = 'e2e-failure: test_scenario_9_posix leg=static rmw=rmw_fastrtps_cpp exit=1'
        output = failure + '\n' + 'test_x ... ok\n' * 2000
        self.assertIn('test_scenario_9_posix', section_report(output))

    def test_names_a_timeout_kill_outside_the_tail(self):
        failure = (
            'e2e-failure: test_scenario_10_posix leg=runtime rmw=rmw_zenoh_cpp exit=124 (timeout)'
        )
        output = failure + '\n' + 'test_x ... ok\n' * 2000
        report = section_report(output)
        self.assertIn('test_scenario_10_posix', report)
        self.assertIn('timeout', report)

    def test_names_a_launch_death_outside_the_tail(self):
        failure = 'e2e-failure: test_scenario_3_posix leg=runtime rmw=rmw_zenoh_cpp exit=1'
        death = (
            'Processes under test stopped before tests completed\n'
            "Process 'bridge-1' exited with 1\n"
        )
        output = failure + '\n' + death + 'test_x ... ok\n' * 2000
        report = section_report(output)
        self.assertIn('test_scenario_3_posix', report)
        self.assertIn('Processes under test stopped before tests completed', report)
        self.assertIn("Process 'bridge-1' exited with 1", report)

    def test_names_an_avr_failure_outside_the_tail(self):
        failure = 'e2e-failure: test_scenario_1_avr board=uno rmw=rmw_fastrtps_cpp exit=1'
        output = failure + '\n' + 'test_x ... ok\n' * 2000
        self.assertIn('test_scenario_1_avr', section_report(output))

    def test_names_an_avr_timeout_kill_outside_the_tail(self):
        failure = (
            'e2e-failure: test_baud_matrix_avr board=mega baud=500000 rmw=rmw_zenoh_cpp exit=124 '
            '(timeout)'
        )
        output = failure + '\n' + 'test_x ... ok\n' * 2000
        report = section_report(output)
        self.assertIn('test_baud_matrix_avr', report)
        self.assertIn('timeout', report)


class TestSectionWallTime(unittest.TestCase):
    def test_names_the_section_and_its_seconds(self):
        logged = io.StringIO()
        with contextlib.redirect_stdout(logged):
            with mock.patch.object(time, 'monotonic', return_value=912.0):
                log_section_wall_time('posix-pair-a', 100.0)
        self.assertIn('posix-pair-a', logged.getvalue())
        self.assertIn('812s', logged.getvalue())


class TestFailureLines(unittest.TestCase):
    def test_names_each_failed_test(self):
        output = 'test_a ... ok\nFAIL: test_b (mod.TestB)\nERROR: test_c (mod.TestC)\n'
        self.assertEqual(
            failure_lines(output), ['FAIL: test_b (mod.TestB)', 'ERROR: test_c (mod.TestC)']
        )

    def test_ignores_passing_sections(self):
        self.assertEqual(failure_lines('test_a ... ok\nOK\n'), [])

    def test_ignores_traceback_body(self):
        output = 'FAIL: test_b (mod.TestB)\nTraceback:\n  File "x.py", line 1\nAssertionError\n'
        self.assertEqual(failure_lines(output), ['FAIL: test_b (mod.TestB)'])

    def test_names_a_dead_launch_process(self):
        output = (
            'test_x ... ok\nProcesses under test stopped before tests completed\n'
            "Process 'bridge-1' exited with 1\n"
        )
        self.assertEqual(
            failure_lines(output),
            [
                'Processes under test stopped before tests completed',
                "Process 'bridge-1' exited with 1",
            ],
        )


def pair_build_dir(suffix):
    """One leg's R2S_BUILD_DIR: separate dirs lock separately, so the legs overlap."""
    base = os.environ.get('R2S_BUILD_DIR', '')
    if not base:
        # Mirrors the run.sh default; run.sh appends e2e/posix and locks build/.lock.
        base = os.path.join(
            os.path.expanduser('~'), '.cache', 'ros2serial', 'build' + str(ROOT).replace('/', '-')
        )
    return base + '-iso-' + suffix


def section_logs_dir():
    """Where full section outputs land for the artifact collector."""
    cache = os.environ.get(
        'R2S_CACHE_DIR', os.path.join(os.path.expanduser('~'), '.cache', 'ros2serial')
    )
    return os.path.join(cache, 'e2e-section-logs')


def write_section_log(name, output):
    """Save one section's full output, returning its path."""
    dest = os.path.join(section_logs_dir(), name + '.log')
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'w', encoding='utf-8') as handle:
        handle.write(output)
    return dest


class TestWriteSectionLog(unittest.TestCase):
    def test_saves_the_full_output_under_the_shared_cache(self):
        with tempfile.TemporaryDirectory() as cache:
            with mock.patch.dict(os.environ, {'R2S_CACHE_DIR': cache}):
                dest = write_section_log('posix-pair-a', 'section says hi\n')
            self.assertEqual(dest, os.path.join(cache, 'e2e-section-logs', 'posix-pair-a.log'))
            with open(dest, encoding='utf-8') as handle:
                self.assertEqual(handle.read(), 'section says hi\n')

    def test_overwrites_the_previous_run(self):
        with tempfile.TemporaryDirectory() as cache:
            with mock.patch.dict(os.environ, {'R2S_CACHE_DIR': cache}):
                first = write_section_log('posix-pair-a', 'old\n')
                second = write_section_log('posix-pair-a', 'new\n')
            self.assertEqual(first, second)
            with open(second, encoding='utf-8') as handle:
                self.assertEqual(handle.read(), 'new\n')


@pytest.mark.skipif(shutil.which('colcon') is None, reason='colcon is not installed')
def test_two_concurrent_e2e_sections_pass_without_crosstalk():
    env_a = dict(
        os.environ,
        R2S_RMWS='rmw_fastrtps_cpp rmw_zenoh_cpp',
        R2S_E2E_SKIP='test_scenario_8_posix',
        R2S_BUILD_DIR=pair_build_dir('a'),
    )
    env_b = dict(
        os.environ,
        R2S_RMWS='rmw_fastrtps_cpp rmw_zenoh_cpp',
        R2S_E2E_SKIP='test_scenario_8_posix',
        R2S_BUILD_DIR=pair_build_dir('b'),
    )
    started_a = time.monotonic()
    first = subprocess.Popen(
        ['bash', 'tests/e2e/run.sh'],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env_a,
    )
    started_b = time.monotonic()
    second = subprocess.Popen(
        ['bash', 'tests/e2e/run.sh'],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env_b,
    )
    out1, _ = first.communicate()
    log_section_wall_time('posix-pair-a', started_a)
    out2, _ = second.communicate()
    log_section_wall_time('posix-pair-b', started_b)
    write_section_log('posix-pair-a', out1)
    write_section_log('posix-pair-b', out2)
    assert first.returncode == 0, 'first section failed:\n' + section_report(out1)
    assert second.returncode == 0, 'second section failed:\n' + section_report(out2)
    assert domain_of(out1) != domain_of(out2), 'both sections ran on one domain'


@pytest.mark.skipif(shutil.which('colcon') is None, reason='colcon is not installed')
def test_avr_and_posix_sections_pass_side_by_side():
    env = dict(
        os.environ,
        R2S_RMWS='rmw_fastrtps_cpp rmw_zenoh_cpp',
        R2S_E2E_BOARDS='uno',
        R2S_E2E_SKIP='test_scenario_8_posix',
    )
    started_avr = time.monotonic()
    avr = subprocess.Popen(
        ['bash', 'tests/e2e/run-avr.sh'],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    started_posix = time.monotonic()
    posix = subprocess.Popen(
        ['bash', 'tests/e2e/run.sh'],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    out_avr, _ = avr.communicate()
    log_section_wall_time('avr-side', started_avr)
    out_posix, _ = posix.communicate()
    log_section_wall_time('posix-side', started_posix)
    write_section_log('avr-side', out_avr)
    write_section_log('posix-side', out_posix)
    assert avr.returncode == 0, 'avr section failed:\n' + section_report(out_avr)
    assert posix.returncode == 0, 'posix section failed:\n' + section_report(out_posix)
    assert domain_of(out_avr) != domain_of(out_posix), 'both sections ran on one domain'
    assert port_of(out_avr) != port_of(out_posix), 'both sections share a zenoh port'
