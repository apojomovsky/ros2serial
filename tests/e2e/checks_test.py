"""Unit tests for the shared scenario checks, with no ROS in the loop.

Run directly: `python3 tests/e2e/checks_test.py`. The launch suite runs it
before any scenario, so a broken helper fails fast instead of mid-handshake.
"""

import contextlib
import io
import struct
import time
import unittest

from e2e_support import checks, teardown


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


class FakeValue:
    """A key/value pair shaped like a diagnostics entry."""

    def __init__(self, key, value):
        self.key = key
        self.value = value


class FakeDiagnostics:
    """A /diagnostics message shaped like the bridge's own."""

    def __init__(self, link_state, entries):
        status = type("S", (), {})()
        status.message = link_state
        status.values = [FakeValue(key, value) for key, value in entries]
        self.status = [status]


class TestHandshakeReady(unittest.TestCase):
    WANTED = ["battery", "cmd_vel"]

    def test_connected_with_all_ok_is_ready(self):
        message = FakeDiagnostics("connected", [("topic battery status", "ok"), ("topic cmd_vel status", "ok")])
        self.assertEqual(checks.handshake_missing(message, self.WANTED), [])

    def test_handshaking_link_is_missing(self):
        message = FakeDiagnostics("handshaking", [])
        self.assertEqual(checks.handshake_missing(message, self.WANTED), ["link handshaking"])

    def test_rejected_topic_is_missing(self):
        message = FakeDiagnostics(
            "connected", [("topic battery status", "ok"), ("topic cmd_vel status", "hash mismatch")]
        )
        self.assertEqual(checks.handshake_missing(message, self.WANTED), ["topic cmd_vel"])

    def test_unlisted_topic_is_missing(self):
        message = FakeDiagnostics("connected", [("topic battery status", "ok")])
        self.assertEqual(checks.handshake_missing(message, self.WANTED), ["topic cmd_vel"])


class TestWaitForHandshake(unittest.TestCase):
    WANTED = ["battery", "cmd_vel"]

    def _ready(self):
        return FakeDiagnostics("connected", [("topic battery status", "ok"), ("topic cmd_vel status", "ok")])

    def test_ready_report_returns_without_spinning(self):
        spins = []
        took = checks.wait_for_handshake(
            [self._ready()], self.WANTED, time.monotonic(), 3.0, "handshake", lambda: spins.append(1)
        )
        self.assertEqual(spins, [])
        self.assertLess(took, 1.0)

    def test_pumps_spin_until_report_completes(self):
        received = [FakeDiagnostics("handshaking", [])]
        ready = self._ready()
        spins = []

        def spin():
            spins.append(1)
            received.append(ready)

        took = checks.wait_for_handshake(received, self.WANTED, time.monotonic(), 3.0, "handshake", spin)
        self.assertEqual(len(spins), 1)
        self.assertGreaterEqual(took, 0.0)

    def test_silent_link_times_out_naming_the_link(self):
        with self.assertRaises(AssertionError) as failure:
            checks.wait_for_handshake([], self.WANTED, time.monotonic() - 10.0, 3.0, "handshake", lambda: None)
        self.assertIn("link silent", str(failure.exception))

    def test_incomplete_handshake_times_out_naming_it(self):
        with self.assertRaises(AssertionError) as failure:
            checks.wait_for_handshake(
                [FakeDiagnostics("handshaking", [])],
                self.WANTED,
                time.monotonic() - 10.0,
                3.0,
                "handshake",
                lambda: None,
            )
        self.assertIn("handshaking", str(failure.exception))

    def test_spin_quantum_stays_short(self):
        self.assertLessEqual(checks.HANDSHAKE_SPIN, 0.05)


class TestDiscoveryTimeout(unittest.TestCase):
    def test_stays_generous_against_the_handshake_bound(self):
        self.assertGreaterEqual(checks.DISCOVERY_TIMEOUT, 10.0)


class TestTeardownShutdown(unittest.TestCase):
    CLOSE_TIMEOUT = "Failed to close the session(Error code: -128)"

    def test_clean_shutdown_calls_through(self):
        called = []
        teardown.shutdown(lambda: called.append(True))
        self.assertEqual(called, [True])

    def test_session_close_timeout_logs_instead_of_raising(self):
        def closing():
            raise RuntimeError(self.CLOSE_TIMEOUT)

        logged = io.StringIO()
        with contextlib.redirect_stderr(logged):
            teardown.shutdown(closing)
        self.assertIn(self.CLOSE_TIMEOUT, logged.getvalue())

    def test_other_shutdown_failure_still_raises(self):
        def failing():
            raise RuntimeError("failed to shutdown: some other cause")

        with self.assertRaises(RuntimeError):
            teardown.shutdown(failing)


if __name__ == "__main__":
    unittest.main()
