"""The assertions the scenarios share across backends.

Each check takes plain data and a deadline, never ROS handles it creates
itself: the simavr backend reuses these with its own launcher. Thresholds
come from docs/01-testing.md section 10, not from measurements.
"""

import math
import time

# covers: SES-9, SES-18, SES-31, FRM-4, FRM-5


def check_float_sequence(values, expected_count, loss_budget, what):
    """A 50 Hz counter stream arrives nearly whole, ordered and integral.

    The firmware publishes 0, 1, 2, ...; a corrupted value would show as a
    non-integer or a step back, and lost frames as a step forward. The
    budget counts lost frames only, so corruption always fails.
    """
    assert len(values) >= expected_count, "%s: only %d of %d messages arrived" % (
        what,
        len(values),
        expected_count,
    )
    for value in values:
        assert float(value).is_integer(), "%s: corrupt value %r" % (what, value)
    for earlier, later in zip(values, values[1:]):
        assert later > earlier, "%s: values out of order: %r after %r" % (what, later, earlier)
    span = int(values[-1]) - int(values[0]) + 1
    assert span - len(values) <= loss_budget, "%s: lost %d of %d" % (what, span - len(values), span)


def check_twist_echo(sent, received, what):
    """Every echoed Twist matches what the host published, field for field.

    The posix build carries 64-bit doubles end to end, so the round trip is
    exact. Narrow backends pass a tolerance instead; the comparison stays in
    one place for both.
    """
    assert len(received) == len(sent), "%s: %d echoes for %d publishes" % (what, len(received), len(sent))
    for want, got in zip(sent, received):
        for axis in ("x", "y", "z"):
            assert math.isclose(getattr(got.linear, axis), getattr(want.linear, axis), rel_tol=0.0, abs_tol=0.0), (
                "%s: linear.%s differs: %r vs %r" % (what, axis, getattr(got.linear, axis), getattr(want.linear, axis))
            )
        for axis in ("x", "y", "z"):
            assert math.isclose(getattr(got.angular, axis), getattr(want.angular, axis), rel_tol=0.0, abs_tol=0.0), (
                "%s: angular.%s differs" % (what, axis)
            )


def collect_until(values, count, deadline, what):
    """Wait until values holds count entries, polling with a timeout."""
    while len(values) < count:
        assert time.monotonic() < deadline, "%s: only %d of %d arrived" % (what, len(values), count)
        time.sleep(0.05)
