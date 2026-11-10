# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Every supported board and example has a stated budget.

The budgets job reports firmware with no entry as unjudged, so a new
board or sketch would land with no promise. This test pins the matrix
the two build scripts produce and requires tools/budgets.yaml to cover
every row; a port that adds a board extends the matrix and the budgets
in the same pull request.
"""

from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

# The sketches scripts/build-examples.sh compiles for Uno and Mega.
AVR_SKETCHES = [
    'arduino_minimal',
    'arduino_action',
    'arduino_hello',
    'arduino_blink',
    'arduino_button',
    'arduino_servo',
    'arduino_range',
    'arduino_xmacro',
    'arduino_static',
    'arduino_xmacro_c',
]

# The sketches scripts/build-examples-esp32.sh compiles for the ESP32.
ESP32_SKETCHES = [
    'arduino_minimal',
    'arduino_hello',
    'arduino_blink',
    'arduino_button',
    'arduino_servo',
    'arduino_range',
    'arduino_xmacro',
    'arduino_static',
    'arduino_xmacro_c',
]


PORT_FIRMWARE = ('stm32/firmware.elf', 'rp2040/firmware.elf')


def expected_firmware() -> list:
    """Every ELF path the example builds produce, keyed like the budgets."""
    names = ['cmake_minimal/hello_cmake']
    for sketch in AVR_SKETCHES:
        for board in ('uno', 'mega'):
            names.append(f'{sketch}/{board}/{sketch}.ino.elf')
    for sketch in ESP32_SKETCHES:
        names.append(f'{sketch}/esp32/{sketch}.ino.elf')
    names.extend(PORT_FIRMWARE)
    return sorted(names)


def test_budgets_cover_every_supported_board_and_example():
    budgets = yaml.safe_load((ROOT / 'tools' / 'budgets.yaml').read_text(encoding='utf-8'))
    missing = [name for name in expected_firmware() if name not in budgets['firmware']]
    assert missing == []
