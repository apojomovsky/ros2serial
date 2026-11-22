# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The ESP32 example build: pinned core, cache data dir, every sketch.

The ESP32 Arduino core lives outside the dev image: the build installs the
version pinned in ci/versions.env into a cache directory (never /opt/arduino)
and compiles every Arduino sketch for the ESP32 Dev Module. The budgets gate
measures the firmware from the same build tree as the AVR firmware.
"""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-examples-esp32.sh'
CORE_SETUP = ROOT / 'scripts' / 'esp32-core.sh'
BUILD = ROOT / 'ports' / 'arduino' / 'build.sh'
BOARDS_LIST = ROOT / 'ports' / 'arduino' / 'boards.list'
VERSIONS = ROOT / 'ci' / 'versions.env'

EXPECTED_BOARDS = (
    ('uno', 'arduino:avr:uno', 'avr'),
    ('mega', 'arduino:avr:mega', 'avr'),
    ('esp32', 'esp32:esp32:esp32', 'xtensa-esp32-elf'),
    ('esp32s3', 'esp32:esp32:esp32s3', 'xtensa-esp32s3-elf'),
    ('esp32s3-usb', 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc', 'xtensa-esp32s3-elf'),
    ('esp32c3', 'esp32:esp32:esp32c3', 'riscv32-esp-elf'),
    ('esp32c3-usb', 'esp32:esp32:esp32c3:CDCOnBoot=cdc', 'riscv32-esp-elf'),
)


def parse_boards_list():
    """The board list as (name, fqbn, prefix) rows, skipping blanks and comments."""
    rows = []
    for line in BOARDS_LIST.read_text(encoding='utf-8').splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        parts = stripped.split()
        assert len(parts) == 3, f'a board line names <name> <fqbn> <prefix>: {line!r}'
        name, fqbn, prefix = parts
        assert name and fqbn and prefix, f'a board line leaves no field empty: {line!r}'
        assert ':' in fqbn, f'a board FQBN names vendor:arch:board: {line!r}'
        rows.append((name, fqbn, prefix))
    names = [name for name, _, _ in rows]
    assert len(names) == len(set(names)), 'the board list names no board twice'
    return rows


def run(args, cwd, tmp_path):
    env = {
        'PATH': '/usr/bin:/bin:/usr/local/bin',
        'HOME': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'b'),
    }
    # A parse regression must fail, never hang the suite.
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=120)


SKETCHES = (
    'arduino_minimal',
    'arduino_hello',
    'arduino_blink',
    'arduino_button',
    'arduino_button_led',
    'arduino_servo',
    'arduino_range',
    'arduino_xmacro',
    'arduino_static',
    'arduino_xmacro_c',
)


def pinned_version():
    for line in VERSIONS.read_text(encoding='utf-8').splitlines():
        if line.startswith('ARDUINO_ESP32_CORE_VERSION='):
            return line.split('=', 1)[1]
    raise AssertionError('ci/versions.env names no ARDUINO_ESP32_CORE_VERSION')


def test_the_core_version_is_pinned_in_versions_env():
    assert pinned_version().count('.') == 2, pinned_version()


def test_the_script_builds_every_sketch_for_the_dev_module():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'esp32:esp32:esp32' in text
    for sketch in SKETCHES:
        assert sketch in text, f'the ESP32 build names no {sketch}'


def test_the_boards_list_names_every_supported_board():
    assert parse_boards_list() == list(EXPECTED_BOARDS)


def test_build_help_names_the_listed_boards_and_only_those(tmp_path):
    listed = [name for name, _, _ in parse_boards_list()]
    result = run(['bash', str(BUILD), '--help'], ROOT, tmp_path)
    assert result.returncode == 0
    text = result.stdout + result.stderr
    match = re.search(r'\[--board ([^\]]+)\]', text)
    assert match, 'the usage names the boards in one [--board ...] group'
    named = [word for word in re.split(r'[|, ]+', match.group(1)) if word]
    assert sorted(named) == sorted(listed)


def test_the_build_sources_the_shared_core_setup_for_esp32():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'scripts/esp32-core.sh' in text
    assert 'core install' not in text
    assert 'core update-index' not in text
    assert 'ci/versions.env' not in text


def test_the_setup_installs_the_pinned_core_from_versions_env():
    text = CORE_SETUP.read_text(encoding='utf-8')
    assert 'ARDUINO_ESP32_CORE_VERSION' in text
    assert 'ci/versions.env' in text
    assert 'core install' in text


def test_the_setup_exports_the_cache_data_directories():
    text = CORE_SETUP.read_text(encoding='utf-8')
    assert 'ARDUINO_DIRECTORIES_DATA' in text
    assert 'ARDUINO_DIRECTORIES_USER' in text


def test_the_data_dir_defaults_to_the_cache_never_the_image():
    text = CORE_SETUP.read_text(encoding='utf-8')
    assert '/opt/arduino' not in text
    assert 'R2S_CACHE_DIR' in text
    assert 'arduino-esp32' in text


def test_the_toolchain_is_located_not_named():
    text = CORE_SETUP.read_text(encoding='utf-8')
    assert 'boards.list' in text
    assert '-size' in text
    assert 'xtensa-esp32-elf-gcc/' not in text


def test_each_compile_keeps_its_own_build_path():
    text = SCRIPT.read_text(encoding='utf-8')
    joined = re.sub(r'\\\n', ' ', text)
    compiles = [line for line in joined.splitlines() if 'arduino-cli compile' in line]
    assert compiles, 'no arduino-cli compile line left to isolate'
    for line in compiles:
        assert '--build-path' in line, f'sketch build shares the arduino cache:\n{line.strip()}'
