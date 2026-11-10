# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Unit tests for the soak checker, with no ROS in the loop.

Run directly: `python3 tests/e2e/soak_check_test.py`. The launch suite runs
it before any scenario, so a broken checker fails fast instead of after a
two hour run. A fixture with counter drift or memory growth must fail the
checker; clean fixtures must pass it.
"""

import unittest

from e2e_support import soak


def clean_counters():
    """A connected link's /diagnostics values after a fault-free run."""
    return {
        'resets': '0',
        'mcu rx frames': '1000',
        'mcu crc errors': '0',
        'mcu rx overflows': '0',
        'mcu dropped frames': '0',
        'mcu error code 1': '0',
        'mcu error code 2': '0',
        'mcu error code 3': '0',
        'mcu error code 4': '0',
        'mcu error code 5': '0',
        'mcu error code 6': '0',
        'topic 16 battery direction': 'device to host',
        'topic 16 battery status': 'ok',
        'topic 16 battery seq gaps': '0',
        'topic 16 battery dropped': '0',
        'topic 16 battery publish failures': '0',
        'topic 16 battery mcu errors': '1=0 2=0 3=0 4=0 5=0 6=0',
    }


def drifted(key, delta=1):
    """Clean counters with one counter moved, as a leak or drop would."""
    values = clean_counters()
    values[key] = str(int(values[key]) + delta)
    return values


class TestWireCounters(unittest.TestCase):
    def test_identical_counters_pass(self):
        soak.check_wire_counters(clean_counters(), clean_counters())

    def test_grown_rx_frames_pass(self):
        # Traffic counters grow by design; only drops and errors must not.
        self.assertEqual(
            soak.check_wire_counters(clean_counters(), drifted('mcu rx frames', 500)), None
        )

    def test_reset_fails(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('resets'))

    def test_mcu_dropped_frames_fail(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('mcu dropped frames'))

    def test_mcu_crc_errors_fail(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('mcu crc errors'))

    def test_mcu_rx_overflows_fail(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('mcu rx overflows'))

    def test_seq_gaps_fail(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('topic 16 battery seq gaps'))

    def test_topic_dropped_fails(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('topic 16 battery dropped'))

    def test_publish_failures_fail(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(
                clean_counters(), drifted('topic 16 battery publish failures')
            )

    def test_mcu_error_code_fails(self):
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), drifted('mcu error code 6'))

    def test_topic_mcu_errors_fail(self):
        moved = clean_counters()
        moved['topic 16 battery mcu errors'] = '1=0 2=0 3=0 4=0 5=0 6=1'
        with self.assertRaises(AssertionError):
            soak.check_wire_counters(clean_counters(), moved)


def flat_samples(n=12, start=0.0, step=10.0, rss=51200.0):
    """Evenly spaced samples at a steady RSS, as a leak-free run shows."""
    return [(start + i * step, rss) for i in range(n)]


class TestMemoryGrowth(unittest.TestCase):
    def test_flat_memory_passes(self):
        soak.check_memory_growth(flat_samples(), 60.0, 5120.0, 'bridge')

    def test_warmup_climb_passes(self):
        # Allocator arenas grow while the link warms up; only later growth counts.
        samples = [(0.0, 40000.0), (30.0, 50000.0)] + flat_samples(10, 60.0, 10.0, 51200.0)
        soak.check_memory_growth(samples, 60.0, 5120.0, 'bridge')

    def test_steady_leak_fails(self):
        samples = flat_samples(6, 0.0, 10.0) + [
            (60.0 + i * 10.0, 51200.0 + i * 2000.0) for i in range(6)
        ]
        with self.assertRaises(AssertionError):
            soak.check_memory_growth(samples, 60.0, 5120.0, 'bridge')

    def test_single_step_after_warmup_fails(self):
        samples = flat_samples(6, 0.0, 10.0) + [(60.0 + i * 10.0, 60000.0) for i in range(6)]
        with self.assertRaises(AssertionError):
            soak.check_memory_growth(samples, 60.0, 5120.0, 'bridge')

    def test_no_post_warmup_samples_fails(self):
        with self.assertRaises(AssertionError):
            soak.check_memory_growth([(0.0, 51200.0)], 60.0, 5120.0, 'bridge')


if __name__ == '__main__':
    unittest.main()
