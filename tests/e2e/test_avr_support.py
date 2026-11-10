# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Unit tests for the simavr backend launcher, with no ROS in the loop.

Run directly: `python3 tests/e2e/test_avr_support.py`. The avr suite runs it
before any scenario, so a broken launcher fails fast instead of mid-handshake.
"""

import ast
import json
import os
import tempfile
import unittest
from unittest import mock

from e2e_support import avr


class TestBoards(unittest.TestCase):
    def test_uno_and_mega_map_to_mcu_and_fqbn(self):
        self.assertEqual(avr.board_mcu('uno'), 'atmega328p')
        self.assertEqual(avr.board_mcu('mega'), 'atmega2560')
        self.assertEqual(avr.board_fqbn('uno'), 'arduino:avr:uno')
        self.assertEqual(avr.board_fqbn('mega'), 'arduino:avr:mega')

    def test_unknown_board_is_rejected(self):
        with self.assertRaises(AssertionError):
            avr.board_mcu('nano')


class TestPtyLine(unittest.TestCase):
    def test_parses_the_harness_pty_announcement(self):
        self.assertEqual(avr.parse_pty_line('r2s_simavr: pty=/dev/pts/3\n'), '/dev/pts/3')

    def test_rejects_other_lines(self):
        with self.assertRaises(AssertionError):
            avr.parse_pty_line(
                'r2s_simavr: stop cycles=1 simulated_ms=2 wall_ms=3 frequency=16000000\n'
            )


class TestHarnessArgv(unittest.TestCase):
    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        fw = os.path.join(self._scratch.name, 'fw')
        os.mkdir(fw)
        harness = os.path.join(self._scratch.name, 'r2s_simavr')
        open(harness, 'w').close()
        for image in (
            'slice-atmega328p.elf',
            'flood-atmega2560.elf',
            'slice-atmega328p-500000.elf',
        ):
            open(os.path.join(fw, image), 'w').close()
        patcher = mock.patch.dict(os.environ, {'R2S_SIMAVR_BIN': harness, 'R2S_E2E_AVR_FW': fw})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_argv_paints_the_stack(self):
        self.assertIn('--stack-paint', avr.harness_argv('uno'))

    def test_argv_names_the_board_mcu_and_slice_firmware(self):
        argv = avr.harness_argv('uno')
        self.assertIn('atmega328p', argv)
        self.assertTrue(any(str(part).endswith('slice-atmega328p.elf') for part in argv))

    def test_flood_flavor_names_the_flood_firmware(self):
        argv = avr.harness_argv('mega', flavor='flood')
        self.assertIn('atmega2560', argv)
        self.assertTrue(any(str(part).endswith('flood-atmega2560.elf') for part in argv))

    def test_baud_names_the_matrix_firmware(self):
        argv = avr.harness_argv('uno', baud='500000')
        self.assertTrue(any(str(part).endswith('slice-atmega328p-500000.elf') for part in argv))

    def test_default_baud_keeps_the_legacy_firmware_name(self):
        argv = avr.harness_argv('uno', baud='115200')
        self.assertTrue(any(str(part).endswith('slice-atmega328p.elf') for part in argv))


class TestFirmwareLabel(unittest.TestCase):
    def test_label_names_flavor_and_mcu(self):
        self.assertEqual(avr.firmware_label('uno'), 'slice-atmega328p')
        self.assertEqual(avr.firmware_label('mega', flavor='flood'), 'flood-atmega2560')

    def test_baud_label_names_the_matrix_rate(self):
        self.assertEqual(avr.firmware_label('uno', baud='500000'), 'slice-atmega328p-500000')

    def test_default_baud_keeps_the_legacy_label(self):
        self.assertEqual(avr.firmware_label('uno', baud='115200'), 'slice-atmega328p')


class TestRecordPeak(unittest.TestCase):
    def test_without_an_output_file_nothing_is_recorded(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('R2S_E2E_AVR_STACK_OUT', None)
            avr.record_peak('slice-atmega328p', 100)

    def test_peaks_merge_to_the_max_per_label(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, 'measured-stack.json')
            with open(path, 'w', encoding='utf-8') as handle:
                handle.write('{}\n')
            with mock.patch.dict(os.environ, {'R2S_E2E_AVR_STACK_OUT': path}):
                avr.record_peak('slice-atmega328p', 100)
                avr.record_peak('slice-atmega328p', 60)
                avr.record_peak('flood-atmega328p', 140)
            with open(path, encoding='utf-8') as handle:
                self.assertEqual(
                    json.load(handle), {'slice-atmega328p': 100, 'flood-atmega328p': 140}
                )

    def test_a_higher_peak_replaces_a_lower_one(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, 'measured-stack.json')
            with open(path, 'w', encoding='utf-8') as handle:
                handle.write('{"slice-atmega328p": 100}\n')
            with mock.patch.dict(os.environ, {'R2S_E2E_AVR_STACK_OUT': path}):
                avr.record_peak('slice-atmega328p', 120)
            with open(path, encoding='utf-8') as handle:
                self.assertEqual(json.load(handle), {'slice-atmega328p': 120})


class FakeProc:
    """A harness process that already exited with one stop line."""

    def __init__(self, tail):
        self._tail = tail
        self.returncode = 0

    def poll(self):
        return 0

    def terminate(self):
        pass

    def communicate(self, timeout=None):
        return self._tail, b''


class TestStopRecordsPeak(unittest.TestCase):
    def test_stop_keeps_the_painted_peak_per_firmware(self):
        tail = (
            b'r2s_simavr: stop cycles=1 simulated_ms=2 wall_ms=3 '
            b'frequency=16000000 stack_peak=77\n'
        )
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, 'measured-stack.json')
            with open(path, 'w', encoding='utf-8') as handle:
                handle.write('{}\n')
            with mock.patch.dict(os.environ, {'R2S_E2E_AVR_STACK_OUT': path}):
                spawned = avr.Spawned(FakeProc(tail), '/dev/pts/9', 0.0, 'slice-atmega328p')
                self.assertEqual(spawned.stop(), 0)
            with open(path, encoding='utf-8') as handle:
                self.assertEqual(json.load(handle), {'slice-atmega328p': 77})


class TestScenariosStopInTest(unittest.TestCase):
    def test_every_avr_scenario_stops_its_harness_in_test(self):
        # launch_testing never calls tearDownModule, so a scenario that
        # stops its harness only there leaks it and records no peak.
        missing = []
        names = [f'test_scenario_{scenario}_avr.py' for scenario in (1, 2, 3, 4, 12)]
        names.append('test_baud_matrix_avr.py')
        for name in names:
            path = os.path.join(os.path.dirname(__file__), name)
            with open(path, encoding='utf-8') as handle:
                tree = ast.parse(handle.read())
            stopped = any(
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == 'stop'
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == '_SPAWNED'
                for node in ast.walk(
                    next(
                        item
                        for item in ast.walk(tree)
                        if isinstance(item, ast.FunctionDef) and item.name.startswith('test_')
                    )
                )
            )
            if not stopped:
                missing.append(path)
        self.assertEqual(
            missing, [], f'scenarios stop their harness only in tearDownModule: {missing}'
        )


class TestStopSummary(unittest.TestCase):
    def test_parses_simulated_ms_from_the_stop_line(self):
        summary = avr.parse_stop_summary(
            'r2s_simavr: stop cycles=160000000 simulated_ms=10042 wall_ms=4214 '
            'frequency=16000000\n'
        )
        self.assertEqual(summary['simulated_ms'], 10042)

    def test_rejects_lines_without_the_stop_prefix(self):
        with self.assertRaises(AssertionError):
            avr.parse_stop_summary('r2s_simavr: pty=/dev/pts/3\n')

    def test_rejects_a_stop_line_without_simulated_ms(self):
        with self.assertRaises(AssertionError):
            avr.parse_stop_summary('r2s_simavr: stop cycles=1 wall_ms=2 frequency=16000000\n')


if __name__ == '__main__':
    unittest.main()
