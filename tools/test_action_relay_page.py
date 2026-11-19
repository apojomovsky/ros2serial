# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The action relay page's host node turns one service call into a goal outcome.

The page carries the node the reader copies, so this test runs that very fence
against a real action server: a goal that succeeds answers the call with
success, one that aborts answers without it, and a call nothing serves is
refused with its own message instead of hanging. A page that stops matching its
node fails here rather than drifting.

The exchange runs in its own process under scripts/e2e-lease.sh, as every test
that puts nodes on a DDS domain does, so two suites never answer each other's
calls.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / 'docs' / 'services-and-parameters.md'
CALL_TIMEOUT = 60
SERVER_TIMEOUT = 20.0

# Drives the page as a reader does: the fence runs as its own program, from
# its own `main()`, so the executor wiring the page prints is the one the call
# crosses. This process holds the action server and the caller.
# `sys.argv[1]` is the outcome to end the goal with, `sys.argv[2]` the fence.
DRIVER = '''import json
import subprocess
import sys
import threading

import rclpy
from rclpy.action import ActionServer
from rclpy.action import CancelResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from tf2_msgs.action import LookupTransform

outcome = sys.argv[1]
SERVER_TIMEOUT = 20.0


class Spin(Node):
    """The action the page sends its goal to, ending it as the run asks."""

    def __init__(self):
        super().__init__('relay_page_spin')
        self.server = ActionServer(
            self, LookupTransform, 'spin', self._execute,
            cancel_callback=lambda _handle: CancelResponse.ACCEPT,
            callback_group=ReentrantCallbackGroup())

    def _execute(self, handle):
        if outcome == 'succeeded':
            handle.succeed()
        else:
            handle.abort()
        return LookupTransform.Result()


def main():
    from std_srvs.srv import Trigger

    # The page's program, run exactly as the reader runs it.
    relay = subprocess.Popen(
        [sys.executable, sys.argv[2]], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)
    rclpy.init()
    spin = Spin() if outcome != 'none' else None
    caller = rclpy.create_node('relay_page_caller')
    executor = MultiThreadedExecutor(num_threads=2)
    executor.add_node(caller)
    if spin is not None:
        executor.add_node(spin)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()
    try:
        client = caller.create_client(Trigger, 'start_spin')
        if not client.wait_for_service(timeout_sec=SERVER_TIMEOUT):
            raise SystemExit('the page program served no start_spin')
        future = client.call_async(Trigger.Request())
        rclpy.spin_until_future_complete(caller, future, timeout_sec=SERVER_TIMEOUT)
        if not future.done():
            raise SystemExit('the page program never answered the call: its executor stalled')
        answer = future.result()
        print(json.dumps({'success': bool(answer.success), 'message': answer.message}))
    finally:
        executor.shutdown()
        thread.join(timeout=SERVER_TIMEOUT)
        relay.terminate()
        try:
            out, err = relay.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            relay.kill()
            out, err = relay.communicate()
        if out or err:
            sys.stderr.write(out + err)
        rclpy.shutdown()


main()
'''


def page_fence() -> str:
    """The page's one python fence: the relay node the reader copies."""
    text = PAGE.read_text(encoding='utf-8')
    blocks = re.findall(r'^```python\n(.*?)^```', text, re.MULTILINE | re.DOTALL)
    assert len(blocks) == 1, f'{PAGE} carries {len(blocks)} python fences, expected one'
    return blocks[0]


@pytest.mark.parametrize(
    ('outcome', 'expected'),
    [
        ('succeeded', {'success': True, 'message': 'succeeded'}),
        ('aborted', {'success': False, 'message': 'aborted'}),
        ('none', {'success': False, 'message': 'no spin server'}),
    ],
)
def test_the_page_node_answers_a_call_from_the_goal_outcome(tmp_path, outcome, expected):
    fence = tmp_path / 'page_relay.py'
    fence.write_text(page_fence(), encoding='utf-8')
    driver = tmp_path / 'drive.py'
    driver.write_text(DRIVER, encoding='utf-8')

    done = subprocess.run(
        [
            'bash',
            str(ROOT / 'scripts' / 'e2e-lease.sh'),
            sys.executable,
            str(driver),
            outcome,
            str(fence),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=CALL_TIMEOUT,
    )

    assert done.returncode == 0, done.stdout + done.stderr
    assert json.loads(done.stdout.strip().splitlines()[-1]) == expected
