# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The Arduino build helper stages the library and compiles the sketch.

`ports/arduino/build.sh` is the README quickstart's single Arduino build
step: it stages the library the manual copy commands used to assemble and
reads the frame limits out of the bundled header itself, so the reader
never composes a `--build-property` flag string.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'ports' / 'arduino' / 'build.sh'
BOARDS_LIST = ROOT / 'ports' / 'arduino' / 'boards.list'


def board_entries():
    """The board list as name to (fqbn, prefix), skipping blanks and comments."""
    entries = {}
    for line in BOARDS_LIST.read_text(encoding='utf-8').splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        parts = stripped.split()
        assert len(parts) == 3, f'a board line names <name> <fqbn> <prefix>: {line!r}'
        name, fqbn, prefix = parts
        assert name not in entries, f'duplicate board {name!r}'
        entries[name] = (fqbn, prefix)
    return entries


def stub_arduino_cli(bin_dir):
    """An arduino-cli stand-in that reports both cores and records the rest.

    `core list` names the pinned cores, so neither the AVR nor the ESP32
    setup downloads anything; every other invocation is appended to
    R2S_ARDUINO_LOG with the PATH it ran with, so the tests prove which
    FQBN compiled and which toolchains were visible.
    """
    bin_dir.mkdir(parents=True, exist_ok=True)
    cli = bin_dir / 'arduino-cli'
    cli.write_text(
        '#!/usr/bin/env bash\n'
        'if [ "$1" = core ] && [ "$2" = list ]; then\n'
        '  echo "arduino:avr 1.8.6"\n'
        '  # The suite runs from the repository root, so the pin reads from\n'
        '  # the same file the shared core setup reads.\n'
        "  pin=$(grep '^ARDUINO_ESP32_CORE_VERSION=' ci/versions.env | cut -d= -f2)\n"
        '  echo "esp32:esp32 $pin"\n'
        '  exit 0\n'
        'fi\n'
        'echo "ARDUINO_CLI $* PATH=$PATH" >> "$R2S_ARDUINO_LOG"\n'
        'exit 0\n'
    )
    cli.chmod(0o755)


def run(args, cwd, tmp_path, extra_env=None):
    env = {
        'PATH': '/usr/bin:/bin:/usr/local/bin',
        'HOME': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'b'),
    }
    if extra_env:
        env.update(extra_env)
    # A parse regression must fail, never hang the suite.
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=120)


def test_the_helper_rejects_an_unknown_board(tmp_path):
    result = run(['bash', str(SCRIPT), '--board', 'bogus'], ROOT, tmp_path)
    assert result.returncode != 0
    assert 'bogus' in result.stderr


def test_the_helper_rejects_an_unknown_board_with_exit_2_and_the_known_ones(tmp_path):
    entries = board_entries()
    result = run(['bash', str(SCRIPT), '--board', 'bogus'], ROOT, tmp_path)
    assert result.returncode == 2
    assert 'bogus' in result.stderr
    for name in entries:
        assert name in result.stderr, f'the error must name the known board {name!r}'


def test_the_build_compiles_mega_with_the_listed_fqbn(tmp_path):
    entries = board_entries()
    bin_dir = tmp_path / 'bin'
    stub_arduino_cli(bin_dir)
    log = tmp_path / 'arduino.log'
    result = run(
        [
            'bash',
            str(SCRIPT),
            '--board',
            'mega',
            '--lib',
            str(tmp_path / 'lib'),
            '--out',
            str(tmp_path / 'fw'),
        ],
        ROOT,
        tmp_path,
        extra_env={
            'PATH': f'{bin_dir}:/usr/bin:/bin:/usr/local/bin',
            'R2S_ARDUINO_LOG': str(log),
        },
    )
    assert result.returncode == 0, result.stderr
    invocation = log.read_text(encoding='utf-8')
    assert 'compile' in invocation
    assert entries['mega'][0] in invocation


def fake_esp32_toolchains(tmp_path):
    """Stand-in ESP32 toolchains under the cache data directory.

    The build resolves its cache from HOME when R2S_CACHE_DIR is unset,
    so the fakes live where the shared core setup searches for <prefix>-size.
    """
    tools = tmp_path / '.cache' / 'ros2serial' / 'arduino-esp32' / 'packages' / 'esp32' / 'tools'
    xtensa = tools / 'esp-x32-test' / 'bin'
    riscv = tools / 'esp-rv32-test' / 'bin'
    xtensa.mkdir(parents=True)
    riscv.mkdir(parents=True)
    for name in ('xtensa-esp32-elf-size', 'xtensa-esp32s3-elf-size'):
        (xtensa / name).write_text('#!/bin/sh\nexit 0\n')
    (riscv / 'riscv32-esp-elf-size').write_text('#!/bin/sh\nexit 0\n')
    return xtensa, riscv


def test_the_build_compiles_esp32_with_the_listed_fqbn_and_toolchains(tmp_path):
    entries = board_entries()
    xtensa_dir, riscv_dir = fake_esp32_toolchains(tmp_path)
    bin_dir = tmp_path / 'bin'
    stub_arduino_cli(bin_dir)
    log = tmp_path / 'arduino.log'
    result = run(
        [
            'bash',
            str(SCRIPT),
            '--board',
            'esp32',
            '--lib',
            str(tmp_path / 'lib'),
            '--out',
            str(tmp_path / 'fw'),
        ],
        ROOT,
        tmp_path,
        extra_env={
            'PATH': f'{bin_dir}:/usr/bin:/bin:/usr/local/bin',
            'R2S_ARDUINO_LOG': str(log),
        },
    )
    assert result.returncode == 0, result.stderr
    invocation = log.read_text(encoding='utf-8')
    assert 'compile' in invocation
    assert entries['esp32'][0] in invocation
    assert str(xtensa_dir) in invocation
    assert str(riscv_dir) in invocation


def test_the_helper_rejects_an_unknown_option(tmp_path):
    result = run(['bash', str(SCRIPT), '--sketchbogus'], ROOT, tmp_path)
    assert result.returncode != 0


def test_the_helper_rejects_a_missing_option_value_without_hanging(tmp_path):
    result = run(['bash', str(SCRIPT), '--board'], ROOT, tmp_path)
    assert result.returncode != 0
    assert 'needs a value' in result.stderr


def test_stage_only_stages_the_library_and_reports_the_limits(tmp_path):
    lib = tmp_path / 'lib'
    result = run(
        ['bash', str(SCRIPT), '--board', 'uno', '--lib', str(lib), '--stage-only'], ROOT, tmp_path
    )
    assert result.returncode == 0, result.stderr
    for name in (
        'library.properties',
        'src/ros2serial.h',
        'src/r2s_bundled.h',
        'src/r2s_bundled.c',
        'src/r2s_arduino_port.cpp',
        'src/r2s_arduino_port.h',
        'src/runtime.cpp',
        'src/r2s/runtime.hpp',
        'src/r2s/r2s_codec.h',
    ):
        assert (lib / name).is_file(), name
    assert not (lib / 'src' / 'test_reset.c').exists()
    assert '-DR2S_WITH_CODEC' in result.stdout
    assert 'R2S_MAX_FRAME=' in result.stdout
    assert 'R2S_MAX_NEST=' in result.stdout


def test_stage_only_reports_a_bad_header_as_an_r2s_gen_error(tmp_path):
    lib = tmp_path / 'lib'
    first = run(
        ['bash', str(SCRIPT), '--board', 'uno', '--lib', str(lib), '--stage-only'], ROOT, tmp_path
    )
    assert first.returncode == 0, first.stderr
    header = lib / 'src' / 'r2s_bundled.h'
    header.write_bytes(header.read_bytes() + b'\xff\n')
    again = run(
        ['bash', str(SCRIPT), '--board', 'uno', '--lib', str(lib), '--stage-only', '--no-bundle'],
        ROOT,
        tmp_path,
    )
    assert again.returncode != 0
    assert 'r2s-gen: error:' in again.stderr


def test_the_quickstart_leads_with_the_uno_and_names_no_build_property():
    text = (ROOT / 'README.md').read_text(encoding='utf-8')
    started = text.index('## Connect your first board')
    body = text[started:]
    # The helper installed by the workspace is the route, not a script a user
    # has to find relative to a checkout.
    assert 'ros2 run r2s_gen r2s-arduino' in body
    assert 'ports/arduino/build.sh' not in body
    # The Uno comes first; the virtual MCU only appears as a later option.
    uno = body.index('Uno')
    assert 'arduino:avr:uno' in body
    posix = body.find('posix')
    assert posix == -1 or uno < posix, (
        'the quickstart must reach the Uno before the posix virtual MCU'
    )
    # Limits ride in with the library, so no build property is ever typed by
    # a user: the helper supplies them.
    assert 'compiler.c.extra_flags' not in body
