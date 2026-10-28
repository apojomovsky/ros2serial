"""Unit tests for the shared scenario checks, with no ROS in the loop.

Run directly: `python3 tests/e2e/checks_test.py`. The launch suite runs it
before any scenario, so a broken helper fails fast instead of mid-handshake.
"""

import unittest

from e2e_support import checks


class Twist:
    """Field-for-field stand-in for geometry_msgs Twist in these tests."""

    def __init__(self, linear, angular):
        self.linear = type("V", (), dict(zip("xyz", linear)))()
        self.angular = type("V", (), dict(zip("xyz", angular)))()


class TestFloatSequence(unittest.TestCase):
    def test_whole_stream_passes(self):
        checks.check_float_sequence([0.0, 1.0, 2.0, 3.0], 4, 0, "battery")

    def test_short_stream_fails(self):
        with self.assertRaises(AssertionError):
            checks.check_float_sequence([0.0, 1.0], 4, 1, "battery")

    def test_loss_within_budget_passes(self):
        checks.check_float_sequence([0.0, 1.0, 3.0, 4.0], 4, 1, "battery")

    def test_loss_over_budget_fails(self):
        with self.assertRaises(AssertionError):
            checks.check_float_sequence([0.0, 5.0, 6.0, 7.0], 4, 1, "battery")

    def test_reorder_fails(self):
        with self.assertRaises(AssertionError):
            checks.check_float_sequence([0.0, 2.0, 1.0, 3.0], 4, 1, "battery")

    def test_fraction_fails(self):
        with self.assertRaises(AssertionError):
            checks.check_float_sequence([0.0, 1.5, 2.0, 3.0], 4, 1, "battery")


class TestTwistEcho(unittest.TestCase):
    def test_matching_echo_passes(self):
        sent = [Twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001))]
        checks.check_twist_echo(sent, [Twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001))], "echo")

    def test_wrong_field_fails(self):
        sent = [Twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001))]
        with self.assertRaises(AssertionError):
            checks.check_twist_echo(sent, [Twist((1.5, -2.25, 3.0), (-0.5, 100.25, -0.001))], "echo")

    def test_missing_echo_fails(self):
        with self.assertRaises(AssertionError):
            checks.check_twist_echo([Twist((1.0, 0.0, 0.0), (0.0, 0.0, 0.0))], [], "echo")


if __name__ == "__main__":
    unittest.main()
