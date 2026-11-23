# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The hardware-check plumbing: the device, its group, and the refusal rules.

`make hardware-check` hands the serial device to the dev image with the group
that owns it, so the caller's uid need not be in dialout. These tests drive
the script with a fake device and a stub interpreter or docker, so the command
it would run is checked without a container, and the usage errors are checked
as text.
"""

import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'hardware-check.sh'

RECORDER = """#!/usr/bin/env bash
printf '%s\\n' "$@" > "$R2S_CALL_LOG"
"""


def stub(bindir, name):
    """A recording stand-in for one executable, first on PATH."""
    path = bindir / name
    path.write_text(RECORDER)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


def run(args, env=None):
    merged = {'PATH': '/usr/bin:/bin:/usr/local/bin', 'HOME': '/tmp', 'R2S_IN_IMAGE': '1'}
    merged.update(env or {})
    return subprocess.run(
        ['bash', str(SCRIPT), *args], cwd=ROOT, env=merged, capture_output=True, text=True
    )


def test_a_missing_board_is_refused_with_the_two_names():
    result = run(['/dev/null', ''])
    assert result.returncode == 2
    assert 'uno or mega' in result.stderr


def test_a_blank_port_is_refused_with_where_to_find_it():
    result = run(['', 'uno'])
    assert result.returncode == 2
    assert 'PORT is required' in result.stderr
    assert 'arduino-cli board list' in result.stderr


def test_an_absent_device_is_refused_before_the_image_runs(tmp_path):
    log = tmp_path / 'call.log'
    result = run(['/nonexistent/ttyACM0', 'uno'], {'R2S_CALL_LOG': str(log)})
    assert result.returncode == 2
    assert 'no device at /nonexistent/ttyACM0' in result.stderr
    assert not log.exists(), 'the checker ran without a device'


def test_inside_the_image_the_checker_gets_the_port_and_the_board(tmp_path):
    device = tmp_path / 'ttyACM0'
    device.write_text('')
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub(bindir, 'python3')
    log = tmp_path / 'call.log'
    result = run(
        [str(device), 'mega'],
        {'PATH': f'{bindir}:/usr/bin:/bin', 'R2S_CALL_LOG': str(log)},
    )
    assert result.returncode == 0, result.stderr
    calls = log.read_text().splitlines()
    assert calls[calls.index('--port') + 1] == str(device)
    assert calls[calls.index('--board') + 1] == 'mega'
    assert calls[0].endswith('hardware_check.py')


def test_outside_the_image_the_device_joins_with_its_own_group(tmp_path):
    device = tmp_path / 'ttyACM0'
    device.write_text('')
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub(bindir, 'docker')
    log = tmp_path / 'docker.log'
    result = run(
        [str(device), 'uno'],
        {
            'PATH': f'{bindir}:/usr/bin:/bin',
            'R2S_IN_IMAGE': '',
            'R2S_CALL_LOG': str(log),
            'R2S_CACHE_DIR': str(tmp_path),
        },
    )
    assert result.returncode == 0, result.stderr
    calls = log.read_text().splitlines()
    assert calls[calls.index('--device') + 1] == str(device)
    group = calls[calls.index('--group-add') + 1]
    assert group.isdigit(), calls
    assert str(device) in calls[calls.index('-v') + 1 :]


def board_names():
    names = []
    for line in (ROOT / 'ports/arduino/boards.list').read_text().splitlines():
        parts = line.split()
        if parts and not parts[0].startswith('#'):
            names.append(parts[0])
    return names


def test_the_usage_lists_every_board_in_the_board_list():
    result = run(['/dev/null', ''])
    assert result.returncode == 2
    for name in board_names():
        assert name in result.stderr


def test_an_unknown_board_is_refused_with_the_known_names():
    result = run(['/dev/null', 'nano'])
    assert result.returncode == 2
    assert 'nano' in result.stderr
    for name in board_names():
        assert name in result.stderr


def fake_vendor(sysfs, dev, vendor):
    node = sysfs / 'class' / 'tty' / dev
    (node / 'device').mkdir(parents=True)
    (node / 'idVendor').write_text(vendor + '\n')


def test_a_usb_board_on_a_non_303a_port_is_refused(tmp_path):
    device = tmp_path / 'ttyUSB0'
    device.write_text('')
    sysfs = tmp_path / 'sys'
    fake_vendor(sysfs, 'ttyUSB0', '0403')
    result = run(
        [str(device), 'esp32s3-usb'],
        {'R2S_SYSFS_ROOT': str(sysfs)},
    )
    assert result.returncode == 2
    assert 'USB' in result.stderr
    assert 'UART' in result.stderr or 'COM' in result.stderr


def test_a_non_usb_esp32_board_on_a_303a_port_is_refused(tmp_path):
    device = tmp_path / 'ttyACM0'
    device.write_text('')
    sysfs = tmp_path / 'sys'
    fake_vendor(sysfs, 'ttyACM0', '303a')
    result = run(
        [str(device), 'esp32s3'],
        {'R2S_SYSFS_ROOT': str(sysfs)},
    )
    assert result.returncode == 2
    assert 'USB' in result.stderr
    assert 'UART' in result.stderr or 'COM' in result.stderr


def test_an_unreadable_sysfs_skips_the_transport_check(tmp_path):
    device = tmp_path / 'ttyACM0'
    device.write_text('')
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub(bindir, 'python3')
    log = tmp_path / 'call.log'
    result = run(
        [str(device), 'esp32s3'],
        {
            'PATH': f'{bindir}:/usr/bin:/bin',
            'R2S_CALL_LOG': str(log),
            'R2S_SYSFS_ROOT': str(tmp_path / 'missing-sys'),
        },
    )
    assert result.returncode == 0, result.stderr


def test_an_avr_board_skips_the_transport_check(tmp_path):
    device = tmp_path / 'ttyACM0'
    device.write_text('')
    sysfs = tmp_path / 'sys'
    fake_vendor(sysfs, 'ttyACM0', '303a')
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub(bindir, 'python3')
    log = tmp_path / 'call.log'
    result = run(
        [str(device), 'uno'],
        {
            'PATH': f'{bindir}:/usr/bin:/bin',
            'R2S_CALL_LOG': str(log),
            'R2S_SYSFS_ROOT': str(sysfs),
        },
    )
    assert result.returncode == 0, result.stderr
