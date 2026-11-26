# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The ESP32 pages agree with the board list, the pin and the validation table."""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def _boards():
    boards = {}
    for line in _read('ports/arduino/boards.list').splitlines():
        parts = line.split()
        if parts and not line.startswith('#'):
            boards[parts[0]] = parts[1]
    return boards


def _esp32_pin():
    for line in _read('ci/versions.env').splitlines():
        if line.startswith('ARDUINO_ESP32_CORE_VERSION='):
            return line.split('=', 1)[1].strip()
    raise AssertionError('ci/versions.env names no ESP32 core pin')


def test_hardware_validation_lists_each_esp32_variant_as_build_in_ci():
    text = _read('docs/hardware-validation.md')
    for row in (
        '| ESP32 | build in CI |',
        '| ESP32-S3 (UART) | build in CI |',
        '| ESP32-S3 (USB CDC) | build in CI |',
        '| ESP32-C3 (USB CDC) | build in CI |',
    ):
        assert row in text


def _board_table(text):
    table = {}
    for line in text.splitlines():
        cells = [cell.strip().strip('`') for cell in line.strip().strip('|').split('|')]
        if len(cells) == 4 and cells[0] in ('ESP32', 'ESP32-S3', 'ESP32-C3'):
            table[cells[0]] = cells[1:]
    return table


def test_esp32_page_covers_board_choice_ports_and_fqbns():
    text = _read('docs/esp32.md')
    boards = _boards()
    assert 'https://espressif.github.io/arduino-esp32/package_esp32_index.json' in text
    assert _esp32_pin() in text
    table = _board_table(text)
    assert table['ESP32'][:2] == ['ESP32 Dev Module', boards['esp32']]
    assert table['ESP32'][2].startswith('none')
    assert table['ESP32-S3'] == [
        'ESP32S3 Dev Module',
        boards['esp32s3'],
        boards['esp32s3-usb'],
    ]
    assert table['ESP32-C3'] == [
        'ESP32C3 Dev Module',
        boards['esp32c3'],
        boards['esp32c3-usb'],
    ]
    assert 'USB CDC On Boot: Disabled' in text
    assert 'USB CDC On Boot: Enabled' in text
    assert 'USB Mode: Hardware CDC and JTAG' in text
    assert '`Serial` is USB' in text
    assert '`Serial0` is UART0' in text
    assert '303a' in text
    assert '/dev/serial/by-id' in text
    assert 'baud is ignored on USB CDC' in text
    intro = ' '.join(text.split('## Install the ESP32 core', 1)[0].split())
    assert 'ESP32-S3 (UART and USB CDC)' in intro
    assert 'ESP32-C3 (USB CDC)' in intro
    assert 'not in the CI example matrix' in text
    assert 'only `esp32c3-usb` is built in CI for the C3' in text
    assert 'vendor alone says' not in text
    assert '/sys/class/tty/' in text
    assert 'with both cables plugged in, both vendors' in ' '.join(text.split())


def test_readme_links_esp32_page_and_matches_validation_level():
    text = _read('README.md')
    assert '[ESP32 boards](docs/esp32.md)' in text
    paragraphs = [' '.join(block.split()) for block in text.split('\n\n')]
    top = next(p for p in paragraphs if p.startswith('**Under active development.**'))
    ports = next(p for p in paragraphs if p.startswith('The bridge targets ROS 2'))
    for paragraph in (top, ports):
        assert 'ESP32, ESP32-S3 (UART and USB CDC)' in paragraph
        assert 'ESP32-C3 (USB CDC)' in paragraph
        assert 'built in CI' in paragraph
    assert 'not that they run on hardware' in top
    assert 'does not establish real-hardware validation' in ports


def test_troubleshooting_covers_the_three_esp32_failures():
    text = _read('docs/troubleshooting.md')
    assert 'CDC On Boot' in text
    assert 'RTS' in text
    assert 'download mode' in text
    assert 'port name changing after a reset' in text.lower()
    assert '/dev/serial/by-id' in text


def test_design_calls_esp32_built_in_ci_not_build_only():
    text = _read('docs/00-design.md')
    assert 'build-only' not in text
    assert text.count('ESP32, S3 and C3 built in CI via the Arduino layout') >= 3
    assert '(no WiFi transport in v1)' in text


def test_changelog_records_the_esp32_docs():
    text = _read('CHANGELOG.md')
    unreleased = text.split('## 0.5.0', 1)[0]
    assert 'ESP32' in unreleased
