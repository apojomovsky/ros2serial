# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The PlatformIO install-and-build gate stages the library and builds it.

`scripts/build-examples-pio.sh` is the X7 acceptance step: it installs the
pinned PlatformIO Core and the pinned Atmel AVR platform into a cache
directory, never into the dev image, stages the platformio layout with the
core and the Arduino port glue as one library, then builds it for Uno and
Mega with `pio ci`. A `--stage-only` run stages the library and reports
the build flags without invoking PlatformIO, so the suite pins the
staging without the downloads.
"""

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-examples-pio.sh'


def run(args, cwd, tmp_path, extra_env=None):
    env = {
        'PATH': '/usr/bin:/bin:/usr/local/bin',
        'HOME': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'b'),
        'R2S_CACHE_DIR': str(tmp_path / 'cache'),
    }
    if extra_env:
        env.update(extra_env)
    # A parse regression must fail, never hang the suite.
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=120)


def stub_pio(bin_dir):
    """A PlatformIO stand-in that records its arguments and succeeds."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    pio = bin_dir / 'pio'
    pio.write_text('#!/usr/bin/env bash\necho "$@" >> "$R2S_PIO_LOG"\nexit 0\n')
    pio.chmod(0o755)
    return pio


def test_the_helper_rejects_an_unknown_board(tmp_path):
    result = run(['bash', str(SCRIPT), '--board', 'bogus'], ROOT, tmp_path)

    assert result.returncode != 0
    assert 'bogus' in result.stderr


def test_the_helper_rejects_an_unknown_option(tmp_path):
    result = run(['bash', str(SCRIPT), '--sketchbogus'], ROOT, tmp_path)

    assert result.returncode != 0


def test_the_helper_rejects_a_missing_option_value_without_hanging(tmp_path):
    result = run(['bash', str(SCRIPT), '--board'], ROOT, tmp_path)

    assert result.returncode != 0
    assert 'needs a value' in result.stderr


def test_the_helper_fails_when_versions_env_names_no_platformio(tmp_path):
    bin_dir = tmp_path / 'bin'
    stub_pio(bin_dir)
    result = run(
        ['bash', str(SCRIPT), '--board', 'uno'],
        ROOT,
        tmp_path,
        extra_env={
            'PATH': f'{bin_dir}:/usr/bin:/bin',
            'R2S_VERSIONS_ENV': str(tmp_path / 'empty-versions.env'),
        },
    )

    assert result.returncode != 0
    assert 'PLATFORMIO_VERSION' in result.stderr


def test_stage_only_stages_the_platformio_library_and_reports_the_flags(tmp_path):
    lib = tmp_path / 'lib'
    result = run(
        ['bash', str(SCRIPT), '--board', 'uno', '--lib', str(lib), '--stage-only'],
        ROOT,
        tmp_path,
    )

    assert result.returncode == 0, result.stderr
    for name in (
        'library.json',
        'src/r2s_generated.h',
        'src/r2s_generated.c',
        'src/r2s_arduino_port.cpp',
        'src/r2s_arduino_port.h',
        'src/r2s/r2s_codec.h',
    ):
        assert (lib / name).is_file(), name
    assert not (lib / 'src' / 'test_reset.c').exists()
    manifest = json.loads((lib / 'library.json').read_text(encoding='utf-8'))
    assert manifest['frameworks'] == 'arduino'
    assert manifest['platforms'] == ['atmelavr', 'espressif32']
    assert '-DR2S_WITH_CODEC' in manifest['build']['flags']
    assert any(flag.startswith('-DR2S_MAX_FRAME=') for flag in manifest['build']['flags'])
    assert '-DR2S_WITH_CODEC' in result.stdout
    assert 'R2S_MAX_FRAME=' in result.stdout


def test_the_build_invokes_pio_ci_for_both_boards(tmp_path):
    bin_dir = tmp_path / 'bin'
    stub_pio(bin_dir)
    log = tmp_path / 'pio.log'
    result = run(
        ['bash', str(SCRIPT)],
        ROOT,
        tmp_path,
        extra_env={
            'PATH': f'{bin_dir}:/usr/bin:/bin',
            'R2S_PIO_LOG': str(log),
            'R2S_PIO_BIN': str(bin_dir / 'pio'),
        },
    )

    assert result.returncode == 0, result.stderr
    assert log.is_file(), 'the build must invoke PlatformIO'
    invocation = log.read_text(encoding='utf-8')
    assert 'ci' in invocation.split()
    assert '--board' in invocation.split()
    assert 'uno' in invocation
    assert 'mega' in invocation
    assert '--lib' in invocation.split()
