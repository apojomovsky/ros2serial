"""The ESP32 example build: pinned core, cache data dir, every sketch.

The ESP32 Arduino core lives outside the dev image: the build installs the
version pinned in ci/versions.env into a cache directory (never /opt/arduino)
and compiles every Arduino sketch for the ESP32 Dev Module. The budgets gate
measures the firmware from the same build tree as the AVR firmware.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-examples-esp32.sh'
VERSIONS = ROOT / 'ci' / 'versions.env'

SKETCHES = (
    'arduino_minimal',
    'arduino_hello',
    'arduino_blink',
    'arduino_button',
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


def test_the_script_installs_the_pinned_core_from_versions_env():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'ARDUINO_ESP32_CORE_VERSION' in text
    assert 'ci/versions.env' in text


def test_the_data_dir_defaults_to_the_cache_never_the_image():
    text = SCRIPT.read_text(encoding='utf-8')
    assert '/opt/arduino' not in text
    assert 'R2S_CACHE_DIR' in text
    assert 'arduino-esp32' in text


def test_the_toolchain_is_located_not_named():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'xtensa-esp32-elf-size' in text
    assert 'xtensa-esp32-elf-gcc/' not in text


def test_each_compile_keeps_its_own_build_path():
    text = SCRIPT.read_text(encoding='utf-8')
    joined = re.sub(r'\\\n', ' ', text)
    compiles = [line for line in joined.splitlines() if 'arduino-cli compile' in line]
    assert compiles, 'no arduino-cli compile line left to isolate'
    for line in compiles:
        assert '--build-path' in line, f'sketch build shares the arduino cache:\n{line.strip()}'
