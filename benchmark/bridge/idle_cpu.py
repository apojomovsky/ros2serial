#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Idle CPU of the bridge over a pty pair, measured where the kernel accounts it.

Question: how much of one core does r2s_bridge burn while the link is quiet?
The bridge hands every byte it reads to the ROS graph, so the cost of the
receive path is the cost of the node: this reads utime+stime from /proc for a
known window instead of sampling a load meter.

Method. One pty pair with the posix firmware on one end and the bridge on the
other, both from the same tree. The handshake completes, the firmware is told
to publish nothing (`--no-pub --no-echo --no-time-now --no-joints
--no-log-text`), and the run waits out a settling window so the burst and the
first corrections are behind it. The measured window then reads the bridge's
own utime+stime twice; the difference over the wall time is the figure.

Two cases, both reported: idle, and 10 Hz, where the firmware publishes one
Float32 on `battery` ten times a second so the same line carries traffic. The
ticket's bar is idle under 2% of a core with 10 Hz within one point of idle.

The script prints a table and, with --json, the same numbers as data. It never
decides pass or fail: the numbers are for the write-up, and a bound judged
here would read as a gate that a slower host could not distinguish from a
regression.
"""

import argparse
import json
import os
import pty
import select
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import rclpy
from diagnostic_msgs.msg import DiagnosticArray

ROOT = Path(__file__).resolve().parent.parent

# The window the figure comes from, and the settle before it: the handshake's
# burst and the first PONGs land in the settle, so the window reads the steady
# state the ticket measures.
SETTLE_S = 5.0
WINDOW_S = 30.0
# How long the handshake may take before the run gives up: the boot wait plus
# a walk of the five announced topics.
READY_TIMEOUT_S = 30.0


class PtyRelay:
    """Two pty slaves joined master to master, so both ends open a tty."""

    def __init__(self):
        self._mcu_master, mcu_slave = pty.openpty()
        self._host_master, host_slave = pty.openpty()
        self.mcu_port = os.ttyname(mcu_slave)
        self.host_port = os.ttyname(host_slave)
        os.close(mcu_slave)
        os.close(host_slave)

    def start(self):
        """Relay both directions until stop() is called."""
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name='pty-relay', daemon=True)
        self._thread.start()

    def _forward(self, source, target):
        # A master reads EIO until its slave is open and after it closes, so a
        # failure here only skips one turn.
        try:
            chunk = os.read(source, 4096)
        except OSError:
            time.sleep(0.005)
            return
        view = memoryview(chunk)
        while view:
            try:
                written = os.write(target, view)
            except OSError:
                time.sleep(0.005)
                return
            view = view[written:]

    def _run(self):
        while not self._stop.is_set():
            readable, _, _ = select.select([self._mcu_master, self._host_master], [], [], 0.05)
            for source in readable:
                target = self._host_master if source == self._mcu_master else self._mcu_master
                self._forward(source, target)

    def stop(self):
        """Join the relay thread and close both masters."""
        self._stop.set()
        self._thread.join(timeout=5.0)
        os.close(self._mcu_master)
        os.close(self._host_master)


def cpu_seconds(pid):
    """utime plus stime of pid, in seconds, from /proc/<pid>/stat."""
    with open(f'/proc/{pid}/stat', encoding='utf-8') as handle:
        fields = handle.read().rsplit(')', 1)[1].split()
    # The fields after the command name start at state, so utime is field 11
    # of the whole line and field 12 is stime; both count clock ticks.
    ticks = int(fields[11]) + int(fields[12])
    return ticks / os.sysconf('SC_CLK_TCK')


def wait_for_link(node, link_states, deadline_s):
    """Spin until /diagnostics reports a connected link, or give up.

    The bridge's own state is what says the measurement window may start: a
    handshake that has not completed yet would put its burst inside the
    window and read as the bridge's idle cost.
    """
    started = time.monotonic()
    while time.monotonic() - started < deadline_s:
        rclpy.spin_once(node, timeout_sec=0.1)
        if link_states and link_states[-1] == 'connected':
            return time.monotonic() - started
    raise SystemExit(f'no connected link within {deadline_s:.0f} s: {link_states[-1:]}')


def open_probe():
    """A node watching the bridge's own link state, with the states it saw."""
    rclpy.init()
    node = rclpy.create_node('r2s_bench_probe')
    states = []

    def on_diagnostics(message):
        if message.status:
            states.append(message.status[0].message)

    node.create_subscription(DiagnosticArray, '/diagnostics', on_diagnostics, 10)
    return node, states


def measure(bridge_bin, firmware_bin, publish_hz, window_s, probe):
    """One case: start both ends, settle, then read the bridge's CPU."""
    relay = PtyRelay()
    relay.start()
    bridge = firmware = None
    node, link_states = probe
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
        args = [
            str(firmware_bin),
            relay.mcu_port,
            '--no-echo',
            '--no-time-now',
            '--no-joints',
            '--no-log-text',
        ]
        if publish_hz > 0:
            args += ['--rate', str(publish_hz)]
        else:
            args.append('--no-pub')
        firmware = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        took = wait_for_link(node, link_states, READY_TIMEOUT_S)
        time.sleep(SETTLE_S)
        before = cpu_seconds(bridge.pid)
        started = time.monotonic()
        time.sleep(window_s)
        elapsed = time.monotonic() - started
        after = cpu_seconds(bridge.pid)
        return {
            'publish_hz': publish_hz,
            'handshake_s': round(took, 2),
            'window_s': round(elapsed, 2),
            'cpu_s': round(after - before, 3),
            'core_pct': round(100.0 * (after - before) / elapsed, 2),
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True, help='the r2s_bridge executable')
    parser.add_argument('--firmware', required=True, help='the posix firmware executable')
    parser.add_argument('--window', type=float, default=WINDOW_S, help='measured seconds')
    parser.add_argument('--json', action='store_true', help='print the rows as JSON')
    args = parser.parse_args()

    probe = open_probe()
    rows = []
    for hz in (0, 10):
        probe[1].clear()
        row = measure(Path(args.bridge), Path(args.firmware), hz, args.window, probe)
        rows.append(row)
        print(
            f'bridge idle CPU: {"idle" if hz == 0 else f"{hz} Hz"}: '
            f'{row["core_pct"]:.2f}% of a core over {row["window_s"]:.1f} s',
            flush=True,
        )
    probe[0].destroy_node()
    rclpy.shutdown()
    if args.json:
        print(json.dumps(rows, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
