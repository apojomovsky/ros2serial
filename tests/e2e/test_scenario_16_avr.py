# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Scenario 16 on avr: the spin action on the emulated slice.

The same goal round trip as the posix scenario: feedback and the result, a
second goal rejected while busy, a cancel ending canceled, and a disconnect
and an MCU reset mid-goal aborting. The bridge opens a stable symlink, so a
respawned harness reconnects on the same path the way scenario 6 replugs.
"""

# covers: SES-35, ACT-2, ACT-3, ACT-4, ACT-5, ACT-6, ACT-8

import os
import tempfile
import time
import unittest
from pathlib import Path

import launch_testing.markers
import rclpy
from action_msgs.msg import GoalStatus
from diagnostic_msgs.msg import DiagnosticArray
from e2e_support import avr as avr_support
from e2e_support import bridge as bridge_support
from e2e_support import checks
from e2e_support import teardown
from rclpy.action import ActionClient
from tf2_msgs.action import LookupTransform

ANNOUNCED = ['/battery', '/cmd_vel', '/echo_vel', '/log_text']
# The bridge reports announced names without the graph's leading slash.
HANDSHAKE_TOPICS = [name.removeprefix('/') for name in ANNOUNCED]
TOPIC_BUDGET = 3.0
ACTION_TIMEOUT = 30.0
GOAL_TIMEOUT = 10.0
FEEDBACK_TIMEOUT = 15.0
RESULT_TIMEOUT = 20.0

_SCRATCH = None
_LINK = None
_SPAWNED = []
_PARAMS = None


def _prepare():
    global _SCRATCH, _LINK, _PARAMS
    _SCRATCH = tempfile.TemporaryDirectory(prefix='r2s-e2e-16-avr-')
    _LINK = Path(_SCRATCH.name) / 'bridge_port'
    _PARAMS = bridge_support.example_params_file()


def _spawn_board():
    # Every test respawns the board: stopping the live harness drops the
    # bridge to DISCONNECTED, and the fresh harness re-handshakes on the
    # repointed link, so each test proves a real reconnect. The module
    # spawn below only gives the bridge a live port at startup; the first
    # test's respawn replaces it the same way.
    for spawned in _SPAWNED:
        spawned.stop()
    board = os.environ.get('R2S_E2E_BOARD', 'uno')
    spawned = avr_support.spawn(board)
    if _LINK.is_symlink() or _LINK.exists():
        _LINK.unlink()
    os.symlink(spawned.pty, _LINK)
    _SPAWNED.append(spawned)
    spawned.ready_at = time.monotonic()
    return spawned


@launch_testing.markers.keep_alive
def generate_test_description():
    _prepare()
    _spawn_board()
    return bridge_support.launch_bridge_only(str(_LINK), _PARAMS)


def tearDownModule():
    for spawned in _SPAWNED:
        spawned.stop()
    if _SCRATCH is not None:
        _SCRATCH.cleanup()


def _wait_link_ready(node, received, since):
    """Spin until the link leaves boot wait, within the generous timeout."""
    while True:
        if received and received[-1].status[0].message not in ('disconnected', 'boot_wait'):
            return max(since, time.monotonic())
        took = time.monotonic() - since
        assert took < bridge_support.HANDSHAKE_TIMEOUT, (
            f'link never left boot wait after {took:.1f} s'
        )
        rclpy.spin_once(node, timeout_sec=0.2)


def _make_goal():
    goal = LookupTransform.Goal()
    goal.target_frame = 'odom'
    goal.source_frame = 'base'
    goal.fixed_frame = ''
    goal.advanced = False
    return goal


class TestScenario16Avr(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        teardown.shutdown()

    def _handshake(self, node):
        # The diagnostics subscription lives until the node dies at the end
        # of the test, so the reset test can keep observing across respawns.
        received = []
        node.create_subscription(DiagnosticArray, '/diagnostics', received.append, 10)
        deadline = time.monotonic() + bridge_support.HANDSHAKE_TIMEOUT
        while not received:
            rclpy.spin_once(node, timeout_sec=0.5)
            self.assertLess(time.monotonic(), deadline, 'no /diagnostics flow from the bridge')
        received.clear()
        spawned = _spawn_board()
        link_ready_at = _wait_link_ready(node, received, spawned.ready_at)
        took = checks.wait_for_handshake(
            received,
            HANDSHAKE_TOPICS,
            link_ready_at,
            TOPIC_BUDGET,
            'handshake',
            lambda: rclpy.spin_once(node, checks.HANDSHAKE_SPIN),
            clock=time.monotonic,
        )
        self.assertLessEqual(took, TOPIC_BUDGET, 'handshake took longer than 3 s')
        handshake_done_at = time.monotonic()
        bridge_support.wait_for_topics(
            node,
            ANNOUNCED,
            handshake_done_at,
            checks.DISCOVERY_TIMEOUT,
            'announced topics',
            lambda: rclpy.spin_once(node, bridge_support.DISCOVERY_SPIN),
        )
        client = ActionClient(node, LookupTransform, '/spin')
        self.assertTrue(
            client.wait_for_server(timeout_sec=ACTION_TIMEOUT),
            'the spin action server never appeared',
        )
        return spawned, client, received

    def _send_goal(self, node, client, feedbacks):
        send_future = client.send_goal_async(
            _make_goal(), feedback_callback=lambda msg: feedbacks.append(msg.feedback)
        )
        deadline = time.monotonic() + GOAL_TIMEOUT
        while not send_future.done():
            rclpy.spin_once(node, timeout_sec=0.05)
            self.assertLess(time.monotonic(), deadline, 'the goal never answered')
        return send_future.result()

    def _await_result(self, node, result_future):
        deadline = time.monotonic() + RESULT_TIMEOUT
        while not result_future.done():
            rclpy.spin_once(node, timeout_sec=0.2)
            self.assertLess(time.monotonic(), deadline, 'the goal never finished')
        return result_future.result()

    def _wait_feedback(self, node, feedbacks):
        deadline = time.monotonic() + FEEDBACK_TIMEOUT
        while not feedbacks:
            rclpy.spin_once(node, timeout_sec=0.05)
            self.assertLess(time.monotonic(), deadline, 'no feedback arrived')

    def test_goal_returns_feedback_and_result(self):
        node = rclpy.create_node('e2e_s16_avr_goal')
        try:
            spawned, client, _ = self._handshake(node)
            try:
                feedbacks = []
                handle = self._send_goal(node, client, feedbacks)
                self.assertTrue(handle.accepted, 'the MCU refused the goal')
                result = self._await_result(node, handle.get_result_async())
                self.assertEqual(result.status, GoalStatus.STATUS_SUCCEEDED)
                self.assertGreaterEqual(len(feedbacks), 1, 'the run sent no feedback')
                transform = result.result.transform
                self.assertEqual(transform.header.frame_id, 'odom')
                self.assertEqual(transform.child_frame_id, 'base')
                self.assertAlmostEqual(transform.transform.translation.x, 1.0)
                self.assertAlmostEqual(transform.transform.translation.y, 2.0)
                self.assertAlmostEqual(transform.transform.translation.z, 3.0)
                self.assertEqual(result.result.error.error, 0)
            finally:
                spawned.stop()
                node.destroy_client(client)
        finally:
            node.destroy_node()

    def test_second_goal_is_rejected_while_busy(self):
        node = rclpy.create_node('e2e_s16_avr_busy')
        try:
            spawned, client, _ = self._handshake(node)
            try:
                first_feedbacks = []
                first = self._send_goal(node, client, first_feedbacks)
                self.assertTrue(first.accepted, 'the MCU refused the first goal')
                self._wait_feedback(node, first_feedbacks)
                second = self._send_goal(node, client, [])
                self.assertFalse(second.accepted, 'the busy action took a second goal')
                result = self._await_result(node, first.get_result_async())
                self.assertEqual(result.status, GoalStatus.STATUS_SUCCEEDED)
            finally:
                spawned.stop()
                node.destroy_client(client)
        finally:
            node.destroy_node()

    def test_cancel_ends_canceled(self):
        node = rclpy.create_node('e2e_s16_avr_cancel')
        try:
            spawned, client, _ = self._handshake(node)
            try:
                feedbacks = []
                handle = self._send_goal(node, client, feedbacks)
                self.assertTrue(handle.accepted, 'the MCU refused the goal')
                # Simulated time can run ahead of the wall clock, so the
                # cancel waits for the first feedback: the goal is still
                # running with most of its window left.
                self._wait_feedback(node, feedbacks)
                cancel_future = handle.cancel_goal_async()
                deadline = time.monotonic() + GOAL_TIMEOUT
                while not cancel_future.done():
                    rclpy.spin_once(node, timeout_sec=0.2)
                    self.assertLess(time.monotonic(), deadline, 'the cancel never answered')
                result = self._await_result(node, handle.get_result_async())
                self.assertEqual(result.status, GoalStatus.STATUS_CANCELED)
            finally:
                spawned.stop()
                node.destroy_client(client)
        finally:
            node.destroy_node()

    def test_disconnect_aborts_the_goal(self):
        node = rclpy.create_node('e2e_s16_avr_drop')
        try:
            spawned, client, _ = self._handshake(node)
            try:
                # Act on accept, not on feedback: simulated time can outrun
                # the wall clock by orders of magnitude, so waiting to
                # observe execution first can miss the goal entirely. The
                # goal is active bridge-side from accept, inside a fifty
                # feedback window.
                handle = self._send_goal(node, client, [])
                self.assertTrue(handle.accepted, 'the MCU refused the goal')
                spawned.stop()
                result = self._await_result(node, handle.get_result_async())
                self.assertEqual(result.status, GoalStatus.STATUS_ABORTED)
            finally:
                spawned.stop()
                node.destroy_client(client)
        finally:
            node.destroy_node()

    def test_reset_aborts_the_goal(self):
        node = rclpy.create_node('e2e_s16_avr_reset')
        try:
            spawned, client, received = self._handshake(node)
            try:
                # Same accept-then-act race as the disconnect test above.
                handle = self._send_goal(node, client, [])
                self.assertTrue(handle.accepted, 'the MCU refused the goal')
                spawned.stop()
                fresh = _spawn_board()
                try:
                    result = self._await_result(node, handle.get_result_async())
                    self.assertEqual(result.status, GoalStatus.STATUS_ABORTED)
                    received.clear()
                    link_ready_at = _wait_link_ready(node, received, fresh.ready_at)
                    took = checks.wait_for_handshake(
                        received,
                        HANDSHAKE_TOPICS,
                        link_ready_at,
                        TOPIC_BUDGET,
                        're-handshake',
                        lambda: rclpy.spin_once(node, checks.HANDSHAKE_SPIN),
                        clock=time.monotonic,
                    )
                    self.assertLessEqual(took, TOPIC_BUDGET, 're-handshake took longer than 3 s')
                    self.assertTrue(
                        client.wait_for_server(timeout_sec=ACTION_TIMEOUT),
                        'the spin action server never came back after the reset',
                    )
                    followups = []
                    followup = self._send_goal(node, client, followups)
                    self.assertTrue(followup.accepted, 'the reset link took no new goal')
                    followup_result = self._await_result(node, followup.get_result_async())
                    self.assertEqual(followup_result.status, GoalStatus.STATUS_SUCCEEDED)
                finally:
                    fresh.stop()
            finally:
                spawned.stop()
                node.destroy_client(client)
        finally:
            node.destroy_node()
