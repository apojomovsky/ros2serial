# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The published budgets page: one generated table the docs check in.

render_page builds docs/budgets.md from fresh measurements, the stack
analysis and tools/budgets.yaml. The budgets job renders the same page
from its own numbers and fails when the checked-in copy differs, so the
table cannot drift from what the tools measure.
"""

import budgets_table

MEASUREMENTS = {
    'arduino_minimal/uno/arduino_minimal.ino.elf': {'flash': 9800, 'ram': 1100},
    'arduino_minimal/esp32/arduino_minimal.ino.elf': {'flash': 270000, 'ram': 73000},
}

STACKS = {
    'arduino_minimal/uno/arduino_minimal.ino.elf': {'total': 150},
}

BUDGETS = {
    'firmware': {
        'arduino_minimal/uno/arduino_minimal.ino.elf': {
            'flash': 12000,
            'ram': 1400,
            'stack': 200,
        },
    }
}


def test_page_names_the_reproduce_command():
    page = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    assert 'make size-report' in page


def test_page_tables_every_measured_firmware():
    page = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    assert 'arduino_minimal/uno/arduino_minimal.ino.elf' in page
    assert 'arduino_minimal/esp32/arduino_minimal.ino.elf' in page


def test_unbudgeted_firmware_reads_na():
    page = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    esp32 = next(line for line in page.splitlines() if 'arduino_minimal/esp32' in line)
    assert 'n/a' in esp32


def test_esp32_stack_reads_na():
    page = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    esp32 = next(line for line in page.splitlines() if 'arduino_minimal/esp32' in line)
    assert esp32.count('n/a') >= 1


def test_page_names_the_features_the_budgets_constrain():
    page = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    assert 'table-driven CRC' in page
    assert 'per-topic narrow' in page


def test_render_is_deterministic():
    first = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    second = budgets_table.render_page(MEASUREMENTS, STACKS, BUDGETS)
    assert first == second
