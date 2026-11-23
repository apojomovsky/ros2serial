#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Wait for the bridge's connected status, then count /button arrivals.

The ESP32 QEMU spike runs the emulated firmware against the bridge and
passes when the bridge reports connected and at least 20 /button messages
arrive within 60 s. Rates and gaps stay out: emulation time is not wall
time, so only the handshake and the count gate the run.
"""

import argparse
import time
from dataclasses import dataclass

WANT_BUTTONS = 20
BUDGET_S = 60.0


@dataclass(frozen=True)
class Observation:
    """What one run heard: when the link connected and how many buttons."""

    connected_s: float | None
    count: int
    elapsed_s: float


def failures(observed, want=WANT_BUTTONS, budget_s=BUDGET_S):
    """Every reason the run fails: no handshake, or too few buttons."""
    if observed.connected_s is None:
        return [f'the bridge reported no connected link within {budget_s:.0f} s']
    if observed.count < want:
        return [
            f'only {observed.count} of {want} /button messages arrived within {budget_s:.0f} s'
        ]
    return []


def format_report(observed, reasons, want=WANT_BUTTONS, budget_s=BUDGET_S):
    """The paste-able report block: the figures, then the verdict."""
    lines = ['== ros2serial esp32-qemu check']
    lines.append(f'want: {want} /button messages within {budget_s:.0f} s')
    if observed.connected_s is None:
        lines.append('handshake: never')
    else:
        lines.append(f'handshake: {observed.connected_s:.1f} s')
    lines.append(f'buttons: {observed.count}')
    lines.append(f'elapsed: {observed.elapsed_s:.1f} s')
    lines += [f'failed: {reason}' for reason in reasons]
    lines.append('verdict: ' + ('FAIL' if reasons else 'PASS'))
    return '\n'.join(lines) + '\n'


def probe(want, budget_s):
    """Wait for the handshake, then count /button until want or the budget."""
    import rclpy
    from diagnostic_msgs.msg import DiagnosticArray
    from rclpy.qos import QoSProfile
    from rclpy.qos import ReliabilityPolicy
    from std_msgs.msg import Bool

    state = {'arrivals': 0, 'connected_at': None}
    rclpy.init()
    node = rclpy.create_node('r2s_esp32_qemu_probe')
    try:
        # The bridge routes MCU topics best effort, so the probe matches it.
        best_effort = QoSProfile(depth=100, reliability=ReliabilityPolicy.BEST_EFFORT)

        def on_diagnostics(message):
            for status in message.status:
                if status.name.endswith(': link') and status.message == 'connected':
                    if state['connected_at'] is None:
                        state['connected_at'] = time.monotonic()

        def on_button(_message):
            if state['connected_at'] is not None:
                state['arrivals'] += 1

        node.create_subscription(DiagnosticArray, '/diagnostics', on_diagnostics, 10)
        node.create_subscription(Bool, '/button', on_button, best_effort)
        started = time.monotonic()
        deadline = started + budget_s
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.2)
            if state['connected_at'] is not None and state['arrivals'] >= want:
                break
        elapsed = time.monotonic() - started
        connected_s = None if state['connected_at'] is None else state['connected_at'] - started
        return Observation(connected_s=connected_s, count=state['arrivals'], elapsed_s=elapsed)
    finally:
        node.destroy_node()
        try:
            rclpy.shutdown()
        except RuntimeError:
            pass


def main(argv=None):
    """Run the probe against the bridge on this port; 0 on PASS, 1 on FAIL."""
    parser = argparse.ArgumentParser(
        description='Count /button messages until want arrive or the budget runs out.'
    )
    parser.add_argument(
        '--want', type=int, default=WANT_BUTTONS, help='how many /button messages pass'
    )
    parser.add_argument(
        '--budget', type=float, default=BUDGET_S, help='how long to wait in seconds'
    )
    args = parser.parse_args(argv)
    observed = probe(args.want, args.budget)
    reasons = failures(observed, args.want, args.budget)
    print(format_report(observed, reasons, args.want, args.budget), end='')
    return 1 if reasons else 0


if __name__ == '__main__':
    raise SystemExit(main())
