# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Unit tests for the ESP32 QEMU probe's verdict, with no ROS and no emulator."""

import esp32_qemu_probe


def passed(connected_s=3.0, count=20, elapsed_s=12.0):
    """A run that connected and heard enough buttons."""
    return esp32_qemu_probe.Observation(connected_s=connected_s, count=count, elapsed_s=elapsed_s)


def test_connected_and_twenty_buttons_pass():
    assert esp32_qemu_probe.failures(passed()) == []


def test_more_buttons_than_wanted_pass():
    assert esp32_qemu_probe.failures(passed(count=137)) == []


def test_a_link_that_never_connected_fails_even_with_buttons():
    reasons = esp32_qemu_probe.failures(passed(connected_s=None, count=25))
    assert len(reasons) == 1
    assert 'connected' in reasons[0]


def test_nineteen_buttons_fail_with_the_count():
    reasons = esp32_qemu_probe.failures(passed(count=19))
    assert len(reasons) == 1
    assert '19' in reasons[0]
    assert '20' in reasons[0]


def test_no_buttons_fail_with_the_count():
    reasons = esp32_qemu_probe.failures(passed(count=0))
    assert '0' in reasons[0]


def test_the_budget_names_sixty_seconds():
    reasons = esp32_qemu_probe.failures(passed(count=0))
    assert '60' in reasons[0]


def test_a_custom_want_and_budget_shape_the_reasons():
    reasons = esp32_qemu_probe.failures(passed(count=4), want=5, budget_s=30.0)
    assert '4' in reasons[0] and '5' in reasons[0] and '30' in reasons[0]


def test_the_report_carries_the_verdict_and_the_figures():
    good = passed(connected_s=2.5, count=21, elapsed_s=9.0)
    report = esp32_qemu_probe.format_report(good, [])
    assert 'verdict: PASS' in report
    assert '2.5' in report
    assert '21' in report
    bad = passed(connected_s=None, count=0, elapsed_s=60.0)
    reasons = esp32_qemu_probe.failures(bad)
    failed = esp32_qemu_probe.format_report(bad, reasons)
    assert 'verdict: FAIL' in failed
