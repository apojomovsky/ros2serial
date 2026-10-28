"""Unit tests for the simavr backend launcher, with no ROS in the loop.

Run directly: `python3 tests/e2e/test_avr_support.py`. The avr suite runs it
before any scenario, so a broken launcher fails fast instead of mid-handshake.
"""

import unittest

from e2e_support import avr


class TestBoards(unittest.TestCase):
    def test_uno_and_mega_map_to_mcu_and_fqbn(self):
        self.assertEqual(avr.board_mcu("uno"), "atmega328p")
        self.assertEqual(avr.board_mcu("mega"), "atmega2560")
        self.assertEqual(avr.board_fqbn("uno"), "arduino:avr:uno")
        self.assertEqual(avr.board_fqbn("mega"), "arduino:avr:mega")

    def test_unknown_board_is_rejected(self):
        with self.assertRaises(AssertionError):
            avr.board_mcu("nano")


class TestPtyLine(unittest.TestCase):
    def test_parses_the_harness_pty_announcement(self):
        self.assertEqual(avr.parse_pty_line("r2s_simavr: pty=/dev/pts/3\n"), "/dev/pts/3")

    def test_rejects_other_lines(self):
        with self.assertRaises(AssertionError):
            avr.parse_pty_line("r2s_simavr: stop cycles=1 simulated_ms=2 wall_ms=3 frequency=16000000\n")


if __name__ == "__main__":
    unittest.main()
