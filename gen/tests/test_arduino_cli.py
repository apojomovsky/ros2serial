# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The installed Arduino entry point supplies a complete, usable library."""

import json
import os

from r2s_gen import arduino


def test_library_contains_runtime_core_port_and_example(tmp_path):
    out = tmp_path / 'library'
    assert arduino.main(['library', '--out', str(out), '--distro', 'jazzy']) == 0
    for name in (
        'src/r2s/r2s.h',
        'src/session.c',
        'src/runtime.cpp',
        'src/r2s_arduino_port.cpp',
        'src/ros2serial/ros.h',
        'src/std_msgs/msg/bool.hpp',
        'examples/button_led/button_led.ino',
    ):
        assert (out / name).is_file(), name
    assert not (out / 'src/test_reset.c').exists()


def test_library_refuses_existing_destination(tmp_path, capsys):
    sentinel = tmp_path / 'my_sketch.ino'
    sentinel.write_text('keep my code')
    assert arduino.main(['library', '--out', str(tmp_path)]) == 1
    assert sentinel.read_text() == 'keep my code'
    assert 'already exists' in capsys.readouterr().err


def test_library_rejects_unknown_distro_without_creating_output(tmp_path):
    out = tmp_path / 'library'
    assert arduino.main(['library', '--out', str(out), '--distro', 'unknown']) != 0
    assert not out.exists()


def test_compile_passes_limits_and_paths_without_shell_expansion(tmp_path, monkeypatch):
    library = tmp_path / 'library with spaces'
    assert arduino.main(['library', '--out', str(library)]) == 0
    fake = tmp_path / 'arduino-cli'
    capture = tmp_path / 'arguments.json'
    fake.write_text(
        '#!/usr/bin/env python3\nimport json, os, sys\n'
        'open(os.environ["CAPTURE"], "w").write(json.dumps(sys.argv[1:]))\n'
        'sys.exit(7)\n'
    )
    fake.chmod(0o755)
    monkeypatch.setenv('PATH', str(tmp_path) + os.pathsep + os.environ['PATH'])
    monkeypatch.setenv('CAPTURE', str(capture))
    sketch = library / 'examples/button_led'
    out = tmp_path / 'firmware'
    result = arduino.main(
        [
            'compile',
            '--library',
            str(library),
            '--fqbn',
            'arduino:avr:uno',
            '--out',
            str(out),
            str(sketch),
        ]
    )
    assert result == 7
    args = json.loads(capture.read_text())
    assert args[0] == 'compile'
    assert args[args.index('--library') + 1] == str(library)
    assert args[-1] == str(sketch)
    for language in ('c', 'cpp'):
        flags = next(arg for arg in args if arg.startswith(f'compiler.{language}.extra_flags='))
        assert '-DR2S_WITH_CODEC' in flags
        assert '-DR2S_MAX_FRAME=' in flags
        assert '-DR2S_MAX_NEST=' in flags


def test_compile_rejects_incomplete_library(tmp_path, capsys):
    assert (
        arduino.main(
            [
                'compile',
                '--library',
                str(tmp_path),
                '--fqbn',
                'arduino:avr:uno',
                '--out',
                str(tmp_path / 'out'),
                str(tmp_path),
            ]
        )
        == 1
    )
    assert 'r2s_bundled.h' in capsys.readouterr().err


def test_compile_reports_missing_arduino_cli(tmp_path, monkeypatch, capsys):
    library = tmp_path / 'library'
    assert arduino.main(['library', '--out', str(library)]) == 0
    monkeypatch.setenv('PATH', '')
    assert (
        arduino.main(
            [
                'compile',
                '--library',
                str(library),
                '--fqbn',
                'arduino:avr:uno',
                '--out',
                str(tmp_path / 'out'),
                str(library / 'examples/button_led'),
            ]
        )
        == 1
    )
    assert 'arduino-cli' in capsys.readouterr().err
