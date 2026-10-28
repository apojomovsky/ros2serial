"""The assertions the scenarios share across backends.

Each check takes plain data and a deadline, never ROS handles it creates
itself: the simavr backend reuses these with its own launcher. Thresholds
come from docs/01-testing.md section 10, not from measurements.
"""

import math
import time

# covers: SES-9, SES-18, SES-31, FRM-4, FRM-5

# Graph discovery of new topics rides DDS, which a loaded runner stretches
# past the handshake: scenarios wait it out here, not against their bounds.
DISCOVERY_TIMEOUT = 20.0


def check_float_sequence(values, expected_count, loss_budget, what):
    """A 50 Hz counter stream arrives nearly whole, ordered and integral.

    The firmware publishes 0, 1, 2, ...; a corrupted value would show as a
    non-integer or a step back, and lost frames as a step forward. The
    budget counts lost frames only, so corruption always fails.
    """
    assert len(values) >= expected_count, f"{what}: only {len(values)} of {expected_count} messages arrived"
    for value in values:
        assert float(value).is_integer(), f"{what}: corrupt value {value!r}"
    for earlier, later in zip(values, values[1:], strict=False):
        assert later > earlier, f"{what}: values out of order: {later!r} after {earlier!r}"
    span = int(values[-1]) - int(values[0]) + 1
    assert span - len(values) <= loss_budget, f"{what}: lost {span - len(values)} of {span}"


def check_twist_echo(sent, received, what):
    """Every echoed Twist matches what the host published, field for field.

    The posix build carries 64-bit doubles end to end, so the round trip is
    exact. Narrow backends pass a tolerance instead; the comparison stays in
    one place for both.
    """
    assert len(received) == len(sent), f"{what}: {len(received)} echoes for {len(sent)} publishes"
    for want, got in zip(sent, received, strict=True):
        for axis in ("x", "y", "z"):
            assert math.isclose(getattr(got.linear, axis), getattr(want.linear, axis)), f"{what}: linear.{axis} differs"
        for axis in ("x", "y", "z"):
            assert math.isclose(getattr(got.angular, axis), getattr(want.angular, axis)), (
                f"{what}: angular.{axis} differs"
            )


def collect_until(values, count, deadline, what, spin):
    """Wait until values holds count entries, pumping the node to do it.

    spin is a zero-argument callable that runs the node's callbacks once;
    without it the subscription filling values would never fire.
    """
    while len(values) < count:
        assert time.monotonic() < deadline, f"{what}: only {len(values)} of {count} arrived"
        spin()


def handshake_missing(message, wanted):
    """What the bridge has not finished handshaking, from one /diagnostics message.

    wanted holds announced names without the graph's leading slash. Empty
    means the link is connected and every wanted topic reads ok.
    """
    status = message.status[0]
    if status.message != "connected":
        return ["link " + status.message]
    values = {value.key: value.value for value in status.values}
    return ["topic " + name for name in wanted if values.get("topic " + name + " status") != "ok"]
