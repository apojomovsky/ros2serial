"""Unit tests for the shared scenario checks, with no ROS in the loop.

Run directly: `python3 tests/e2e/checks_test.py`. The launch suite runs it
before any scenario, so a broken helper fails fast instead of mid-handshake.
"""

import struct
import time
import unittest

from e2e_support import checks


class Twist:
    """Field-for-field stand-in for geometry_msgs Twist in these tests."""

    def __init__(self, linear, angular):
        self.linear = type("V", (), dict(zip("xyz", linear, strict=True)))()
        self.angular = type("V", (), dict(zip("xyz", angular, strict=True)))()


def narrow_floats(values):
    """What an 8-bit MCU keeps of each double: the float32 round trip."""
    return [struct.unpack("<f", struct.pack("<f", value))[0] for value in values]


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


class TestTwistEchoNarrow(unittest.TestCase):
    def test_narrow_round_trip_passes_with_tolerance(self):
        sent = [Twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001))]
        echoed = [Twist(narrow_floats((1.5, -2.25, 3.125)), narrow_floats((-0.5, 100.25, -0.001)))]
        checks.check_twist_echo(sent, echoed, "echo", rel_tol=1e-6, abs_tol=1e-9)

    def test_narrow_tolerance_still_rejects_corruption(self):
        sent = [Twist((1.5, -2.25, 3.125), (-0.5, 100.25, -0.001))]
        corrupted = [Twist((1.5, -2.25, 3.0), (-0.5, 100.25, -0.001))]
        with self.assertRaises(AssertionError):
            checks.check_twist_echo(sent, corrupted, "echo", rel_tol=1e-6, abs_tol=1e-9)


class TestCollectUntil(unittest.TestCase):
    def test_pumps_until_count_reached(self):
        seen = []
        checks.collect_until(seen, 3, time.monotonic() + 5.0, "probe", lambda: seen.append(1.0))
        self.assertEqual(seen, [1.0, 1.0, 1.0])

    def test_times_out_when_nothing_arrives(self):
        with self.assertRaises(AssertionError):
            checks.collect_until([], 1, time.monotonic() - 1.0, "probe", lambda: None)


if __name__ == "__main__":
    unittest.main()
