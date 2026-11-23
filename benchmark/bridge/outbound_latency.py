#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Host-to-MCU latency over a pty pair, publish to echo.

Question: how long does a ROS message wait before the wire carries it? The
receive path already wakes on arrival, so the round trip is dominated by the
transmit half: a publish on /cmd_vel crosses the bridge, the posix firmware
echoes it on /echo_vel, and the bridge forwards the echo. This sends N Twists
on an idle link and reports the publish-to-echo delay as p50/p99.

Method. One pty pair with the posix firmware on one end and the bridge on the
other, both from the same tree. The firmware publishes nothing itself, so the
only traffic is the handshake, the heartbeats and the samples. Each sample
carries its index in linear.x, which round-trips exactly, so an echo is
matched to its send time without clock tricks.

The script prints a table and, with --json, the same numbers as data. It never
decides pass or fail: the numbers are for the write-up, and a bound judged
here would read as a gate that a slower host could not distinguish from a
regression.
"""

import argparse
import importlib.util
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import rclpy
from geometry_msgs.msg import Twist
from rclpy.qos import QoSProfile, ReliabilityPolicy

HERE = Path(__file__).resolve().parent

# The idle link between samples, so each one measures a wakeup and not a
# queue: at this spacing the pacer is always empty when a sample leaves.
SAMPLE_PERIOD_S = 0.5
# How many publish-to-echo delays judge one run: enough for a p99 with ten
# samples above it, few enough to finish in a minute.
SAMPLES = 50
# How long the handshake may take before the run gives up: the boot wait plus
# a walk of the five announced topics.
READY_TIMEOUT_S = 30.0
# How long one echo may take before the run gives up: the link answers in
# milliseconds, so seconds mean the sample was lost, not delayed.
SAMPLE_TIMEOUT_S = 10.0
SETTLE_S = 5.0


def load_idle_cpu():
    """Reuse the pty relay and link-wait helpers from the idle CPU script."""
    spec = importlib.util.spec_from_file_location('r2s_idle_cpu', HERE / 'idle_cpu.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def percentile(sorted_rows, pct):
    """Nearest-rank percentile of sorted rows, in milliseconds."""
    rank = min(len(sorted_rows), max(1, round(pct / 100.0 * len(sorted_rows))))
    return sorted_rows[rank - 1]


def measure(bridge_bin, firmware_bin, samples, period_s, helpers):
    """One run: start both ends, settle, then time publish-to-echo delays."""
    relay = helpers.PtyRelay()
    relay.start()
    bridge = firmware = None
    node, link_states = helpers.open_probe()
    # The bridge forwards MCU traffic best effort in both directions, so the
    # probe matches that; reliable ends never match.
    best_effort = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT)
    pub = node.create_publisher(Twist, '/cmd_vel', best_effort)
    echoes = []

    def on_echo(message):
        echoes.append((message.linear.x, time.monotonic()))

    node.create_subscription(Twist, '/echo_vel', on_echo, best_effort)
    try:
        bridge = subprocess.Popen(
            [
                str(bridge_bin),
                '--ros-args',
                '-p',
                f'port:={relay.host_port}',
                '-p',
                'boot_wait_ms:=500',
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=dict(os.environ),
        )
        firmware = subprocess.Popen(
            [
                str(firmware_bin),
                relay.mcu_port,
                '--no-pub',
                '--no-time-now',
                '--no-joints',
                '--no-log-text',
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        helpers.wait_for_link(node, link_states, READY_TIMEOUT_S)
        time.sleep(SETTLE_S)
        sent = {}
        delays = []
        for index in range(1, samples + 1):
            message = Twist()
            message.linear.x = float(index)
            sent[float(index)] = time.monotonic()
            pub.publish(message)
            deadline = time.monotonic() + SAMPLE_TIMEOUT_S
            while time.monotonic() < deadline:
                # Spin without sleeping: spin_once wakes on arrival, so the
                # echo is timestamped when it lands and not on a poll quantum.
                rclpy.spin_once(node, timeout_sec=0.05)
                hits = [at for key, at in echoes if key == float(index)]
                if hits:
                    delays.append((hits[0] - sent[float(index)]) * 1000.0)
                    break
            else:
                raise SystemExit(f'sample {index} never echoed back')
            time.sleep(period_s)
        delays.sort()
        return {
            'samples': len(delays),
            'p50_ms': round(percentile(delays, 50), 2),
            'p99_ms': round(percentile(delays, 99), 2),
            'max_ms': round(delays[-1], 2),
        }
    finally:
        for proc in (firmware, bridge):
            if proc is not None and proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        relay.stop()
        node.destroy_node()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True, help='the r2s_bridge executable')
    parser.add_argument('--firmware', required=True, help='the posix firmware executable')
    parser.add_argument('--samples', type=int, default=SAMPLES, help='echoes to time')
    parser.add_argument(
        '--period', type=float, default=SAMPLE_PERIOD_S, help='seconds between samples'
    )
    parser.add_argument('--json', action='store_true', help='print the row as JSON')
    args = parser.parse_args()
    helpers = load_idle_cpu()
    try:
        row = measure(args.bridge, args.firmware, args.samples, args.period, helpers)
    finally:
        rclpy.shutdown()
    if args.json:
        print(json.dumps(row))
    else:
        print(f'samples: {row["samples"]}')
        print(f'p50: {row["p50_ms"]} ms')
        print(f'p99: {row["p99_ms"]} ms')
        print(f'max: {row["max_ms"]} ms')


if __name__ == '__main__':
    sys.exit(main())
