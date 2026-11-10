# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

# The probe's pause between /diagnostics reads: the reading lands within
# one short spin of the report's arrival instead of a 0.5 s poll quantum.
# The remaining bias of up to one diagnostics period is accepted by decision.
HANDSHAKE_SPIN = 0.05


def check_float_sequence(values, expected_count, loss_budget, what):
    """A 50 Hz counter stream arrives nearly whole, ordered and integral.

    The firmware publishes 0, 1, 2, ...; a corrupted value would show as a
    non-integer or a step back, and lost frames as a step forward. The
    budget counts lost frames only, so corruption always fails.
    """
    assert len(values) >= expected_count, (
        f'{what}: only {len(values)} of {expected_count} messages arrived'
    )
    for value in values:
        assert float(value).is_integer(), f'{what}: corrupt value {value!r}'
    for earlier, later in zip(values, values[1:], strict=False):
        assert later > earlier, f'{what}: values out of order: {later!r} after {earlier!r}'
    span = int(values[-1]) - int(values[0]) + 1
    assert span - len(values) <= loss_budget, f'{what}: lost {span - len(values)} of {span}'


def check_twist_echo(sent, received, what, *, rel_tol=1e-9, abs_tol=0.0):
    """Every echoed Twist matches what the host published, field for field.

    The posix build carries 64-bit doubles end to end, so the round trip is
    exact. Narrow backends pass a tolerance instead; the comparison stays in
    one place for both.
    """
    assert len(received) == len(sent), f'{what}: {len(received)} echoes for {len(sent)} publishes'
    for want, got in zip(sent, received, strict=True):
        for axis in ('x', 'y', 'z'):
            assert math.isclose(
                getattr(got.linear, axis),
                getattr(want.linear, axis),
                rel_tol=rel_tol,
                abs_tol=abs_tol,
            ), f'{what}: linear.{axis} differs'
        for axis in ('x', 'y', 'z'):
            assert math.isclose(
                getattr(got.angular, axis),
                getattr(want.angular, axis),
                rel_tol=rel_tol,
                abs_tol=abs_tol,
            ), f'{what}: angular.{axis} differs'


def collect_until(values, count, deadline, what, spin):
    """Wait until values holds count entries, pumping the node to do it.

    spin is a zero-argument callable that runs the node's callbacks once;
    without it the subscription filling values would never fire.
    """
    while len(values) < count:
        assert time.monotonic() < deadline, f'{what}: only {len(values)} of {count} arrived'
        spin()


def wait_for_handshake(received, wanted, since, budget, what, spin, clock=time.monotonic):
    """Block until the latest /diagnostics report shows the handshake done.

    The latest report is read before spinning, so the time kept is the
    arrival of the completing report, never a spin quantum later. spin is
    a zero-argument callable that runs the node's callbacks once, pausing
    HANDSHAKE_SPIN between reads. clock reads the clock since came from,
    so callers pass their own time.monotonic and driven tests with a
    stepped clock measure fake seconds. Returns the seconds from since.
    """
    while True:
        missing = handshake_missing(received[-1], wanted) if received else ['link silent']
        took = clock() - since
        if not missing:
            return took
        assert took < budget, f'{what} still missing {missing} after {took:.1f} s'
        spin()


def topic_key(values, name, field):
    """The full diagnostics key for one announced topic's field.

    Per-topic keys read `topic <id> <name> <field>`; the id keeps two
    topics sharing one name apart. Callers name the topic and field and
    this resolves the id, failing loudly on zero or several matches.
    """
    matches = [
        key
        for key in values
        if key.startswith('topic ') and key.endswith(' ' + name + ' ' + field)
    ]
    assert len(matches) == 1, (
        f'expected one key for topic {name!r} field {field!r}, found {matches}'
    )
    return matches[0]


def handshake_missing(message, wanted):
    """What the bridge has not finished handshaking, from one /diagnostics message.

    wanted holds announced names without the graph's leading slash. Empty
    means the link is connected and every wanted topic reads ok. A name
    announced twice needs every entry ok: a refused duplicate still means
    the handshake did not finish that name.
    """
    status = message.status[0]
    if status.message != 'connected':
        return ['link ' + status.message]
    values = {value.key: value.value for value in status.values}
    missing = []
    for name in wanted:
        verdicts = [
            value
            for key, value in values.items()
            if key.startswith('topic ') and key.endswith(' ' + name + ' status')
        ]
        if not verdicts or any(verdict != 'ok' for verdict in verdicts):
            missing.append('topic ' + name)
    return missing
