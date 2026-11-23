# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The ESP32 QEMU spike: firmware under emulation against the bridge.

`scripts/esp32-qemu-check.sh` builds `arduino_button_led` for the ESP32 Dev
Module, merges its flash image, and runs it under Espressif's QEMU against
the bridge, passing when the bridge reports connected and at least 20
/button messages arrive within 60 s. The QEMU release stays pinned by
version and SHA-256 in ci/versions.env, and `docker/esp32-qemu/Dockerfile`
layers its binary onto the dev image without changing the dev image itself.
Rates and gaps are not asserted under emulation. These tests drive the
script with stub executables where no board, firmware or emulator is
needed, and read the rest as text.
"""

import re
import shutil
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASH = shutil.which('bash')
SCRIPT = ROOT / 'scripts' / 'esp32-qemu-check.sh'
DOCKERFILE = ROOT / 'docker' / 'esp32-qemu' / 'Dockerfile'
VERSIONS = ROOT / 'ci' / 'versions.env'


def pins():
    """The ESP_QEMU_* pins the script and the image both read."""
    found = {}
    for line in VERSIONS.read_text(encoding='utf-8').splitlines():
        if line.startswith('ESP_QEMU_VERSION=') or line.startswith('ESP_QEMU_SHA256='):
            key, _, value = line.partition('=')
            found[key] = value
    return found


def test_the_qemu_release_is_pinned_by_version_and_hash():
    found = pins()
    assert re.fullmatch(r'esp-develop-\d+\.\d+\.\d+-\d{8}', found['ESP_QEMU_VERSION']), found
    assert re.fullmatch(r'[0-9a-f]{64}', found['ESP_QEMU_SHA256']), found


def test_the_dockerfile_layers_the_pinned_binary_onto_the_dev_image():
    text = DOCKERFILE.read_text(encoding='utf-8')
    assert 'FROM ${DEV_IMAGE}' in text
    assert 'ARG DEV_IMAGE' in text
    for pin in ('ESP_QEMU_VERSION', 'ESP_QEMU_SHA256'):
        assert pin in text, f'the Dockerfile names no {pin}'
    assert 'qemu-xtensa-softmmu' in text
    assert 'qemu-system-xtensa' in text
    assert 'sha256sum' in text
    assert text.count('FROM ') == 1, 'the image layers once, onto the dev image'


def test_the_dockerfile_checks_the_binary_runs():
    text = DOCKERFILE.read_text(encoding='utf-8')
    assert 'qemu-system-xtensa --version' in text


def test_the_script_builds_button_led_for_the_esp32_dev_module():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'arduino_button_led' in text
    assert 'esp32:esp32:esp32' in text or 'build-examples-esp32' in text


def test_the_script_takes_the_cores_merged_flash_image():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'arduino_button_led.ino.merged.bin' in text


def test_the_script_pads_a_short_image_to_four_megabytes():
    text = SCRIPT.read_text(encoding='utf-8')
    assert '4194304' in text


def test_the_script_runs_the_ticket_qemu_command():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'qemu-system-xtensa' in text
    assert '-M esp32' in text
    assert '-nographic' in text
    assert 'if=mtd,format=raw' in text
    assert '-serial pty' in text


def test_the_script_passes_on_connected_and_twenty_buttons_in_sixty_seconds():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'connected' in text
    assert '20' in text
    assert '60' in text


def test_the_script_asserts_no_rate_or_gap_under_emulation():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'RATE_TOLERANCE' not in text
    assert 'GAP_MS' not in text


RECORDER = """#!/usr/bin/env bash
printf '%s\\n' "$@" >> "$R2S_CALL_LOG"
exit "${R2S_STUB_STATUS:-0}"
"""


def stub(bindir, name):
    """A recording stand-in for one executable, first on PATH."""
    path = bindir / name
    path.write_text(RECORDER, encoding='utf-8')
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def run(args, env=None):
    merged = {'PATH': '/usr/bin:/bin:/usr/local/bin', 'HOME': '/tmp'}
    merged.update(env or {})
    assert BASH is not None, 'the test needs bash on PATH'
    return subprocess.run(
        [BASH, str(SCRIPT), *args], cwd=ROOT, env=merged, capture_output=True, text=True
    )


def test_an_extra_argument_is_a_usage_error():
    result = run(['--sketch'])
    assert result.returncode == 2
    assert 'usage' in result.stderr


def test_outside_the_image_the_script_builds_then_runs_the_qemu_image(tmp_path):
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub(bindir, 'docker')
    log = tmp_path / 'call.log'
    home = tmp_path / 'home'
    home.mkdir()
    result = run(
        [],
        {
            'PATH': f'{bindir}:/usr/bin:/bin',
            'HOME': str(home),
            'R2S_CALL_LOG': str(log),
        },
    )
    assert result.returncode == 0, result.stderr
    calls = log.read_text(encoding='utf-8').splitlines()
    build = calls[: calls.index('run')]
    assert any('docker/esp32-qemu/Dockerfile' in call for call in build)
    assert any('ros2serial-esp32-qemu' in call for call in build)
    assert any('ESP_QEMU_VERSION=' in call for call in build), build
    assert any('ESP_QEMU_SHA256=' in call for call in build), build
    run_calls = calls[calls.index('run') + 1 :]
    assert any('scripts/esp32-qemu-check.sh' in call for call in run_calls), run_calls


def test_without_docker_the_host_path_names_it(tmp_path):
    home = tmp_path / 'home'
    home.mkdir()
    result = run([], {'PATH': '/usr/bin:/bin', 'HOME': str(home)})
    assert result.returncode != 0
    assert 'docker' in result.stderr


def test_inside_the_image_without_qemu_the_script_names_it(tmp_path):
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    result = run(
        [],
        {
            'PATH': str(bindir),
            'HOME': '/tmp',
            'R2S_IN_IMAGE': '1',
        },
    )
    assert result.returncode != 0
    assert 'qemu-system-xtensa' in result.stderr
