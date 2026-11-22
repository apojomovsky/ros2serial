# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The release artifacts: an Arduino library that compiles, and the Python sdist and wheel."""

import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-release-artifacts.sh'

sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen import layouts  # noqa: E402


def sdist_from_clean_copy(tmp_path: Path) -> tuple[Path, Path]:
    """An sdist built from a clean copy of the tree, as the ticket reproduces it."""
    work = tmp_path / 'work'
    shutil.copytree(
        ROOT / 'gen',
        work / 'gen',
        ignore=shutil.ignore_patterns(
            '__pycache__', '*.egg-info', 'build', 'dist', 'firmware_stage'
        ),
    )
    shutil.copytree(ROOT / 'cpp', work / 'cpp', ignore=shutil.ignore_patterns('__pycache__'))
    for directory in ('core/include', 'core/src', 'ports/arduino', 'examples'):
        shutil.copytree(ROOT / directory, work / directory)
    dist = tmp_path / 'dist'
    dist.mkdir()
    result = subprocess.run(
        ['python3', 'setup.py', '-q', 'sdist', '--dist-dir', str(dist)],
        cwd=work / 'gen',
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    (tarball,) = sorted(dist.glob('r2s_gen-*.tar.gz'))
    return work, tarball


def build(tmp_path: Path, *args: str) -> tuple[Path, subprocess.CompletedProcess]:
    out = tmp_path / 'dist'
    result = subprocess.run(
        ['bash', str(SCRIPT), '--out', str(out), *args], capture_output=True, text=True
    )
    return out, result


def test_the_arduino_zip_stages_the_bundle_and_the_core(tmp_path):
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob('ros2serial-arduino-*.zip'))
    names = zipfile.ZipFile(zip_path).namelist()
    for name in (
        'ros2serial/library.properties',
        'ros2serial/src/ros2serial.h',
        'ros2serial/src/r2s_bundled.h',
        'ros2serial/src/r2s_bundled.c',
        'ros2serial/src/r2s/r2s_codec.h',
        'ros2serial/src/session.c',
        'ros2serial/src/r2s/runtime.hpp',
        'ros2serial/src/r2s/service.hpp',
        'ros2serial/src/r2s/action.hpp',
        'ros2serial/src/runtime.cpp',
        'ros2serial/src/service.cpp',
        'ros2serial/src/action.cpp',
        'ros2serial/src/r2s_arduino_port.cpp',
        'ros2serial/src/std_msgs/msg/float32.hpp',
    ):
        assert name in names, name
    assert not [name for name in names if name.endswith('test_reset.c')]
    assert sorted(out.glob('r2s_gen-*.tar.gz')) and sorted(out.glob('r2s_gen-*.whl'))


def test_the_arduino_zip_is_the_platformio_artifact(tmp_path):
    """PlatformIO installs Arduino-format libraries, so the release ships no
    separate PlatformIO package: the Arduino zip carries the manifest, the
    sources under src/ and the flags the PlatformIO layout expects."""
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob('ros2serial-arduino-*.zip'))
    staged = tmp_path / 'pio'
    with zipfile.ZipFile(zip_path) as archive:
        archive.extractall(staged)
    lib = staged / 'ros2serial'
    props = (lib / 'library.properties').read_text(encoding='utf-8').splitlines()
    version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    assert 'name=ros2serial' in props
    assert f'version={version}' in props
    assert 'architectures=avr,esp32' in props
    files = [path for path in lib.rglob('*') if path.is_file()]
    assert files, 'the zip stages no files'
    # An Arduino library keeps its sketches beside src/, which is what the
    # IDE's File > Examples menu lists.
    assert not [
        path
        for path in files
        if path.relative_to(lib).parts[0] not in ('src', 'examples')
        and path.name != 'library.properties'
    ], 'every staged file but the manifest sits under src/ or examples/'
    for name in (
        'src/ros2serial.h',
        'src/r2s_bundled.h',
        'src/r2s_bundled.c',
        'src/r2s_arduino_port.cpp',
        'src/r2s_arduino_port.h',
        'src/r2s/r2s_codec.h',
        'src/r2s/service.hpp',
        'src/r2s/action.hpp',
        'src/runtime.cpp',
        'src/service.cpp',
        'src/action.cpp',
    ):
        assert (lib / name).is_file(), name
    defines = layouts.build_defines((lib / 'src/r2s_bundled.h').read_text(encoding='utf-8'))
    assert 'R2S_WITH_CODEC' in defines
    for prefix in ('R2S_MAX_FRAME=', 'R2S_MAX_NEST='):
        matches = [define for define in defines if define.startswith(prefix)]
        assert len(matches) == 1, defines
        assert matches[0].removeprefix(prefix).isdigit(), defines


SHIPPED_EXAMPLES = (
    'hello',
    'blink',
    'button',
    'button_led',
    'minimal',
    'range',
    'servo',
    'static',
    'xmacro',
    'xmacro_c',
)


def test_the_arduino_zip_ships_the_examples_a_first_run_opens(tmp_path):
    """An Arduino library lists examples/ at the IDE's File > Examples menu."""
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob('ros2serial-arduino-*.zip'))
    names = zipfile.ZipFile(zip_path).namelist()

    for name in SHIPPED_EXAMPLES:
        assert f'ros2serial/examples/{name}/{name}.ino' in names, name
    # The channel examples need a feature flag and a frame limit, so the
    # shipped library does not configure them and they stay out.
    for name in ('action', 'service', 'xmacro_custom', 'pio_hello'):
        assert not [entry for entry in names if f'/examples/{name}/' in entry], name
    assert 'ros2serial/src/r2s_config.h' in names


def readme_ide_section() -> str:
    """The README's Arduino IDE block, from its heading to the next one."""
    lines = (ROOT / 'README.md').read_text(encoding='utf-8').splitlines()
    start = lines.index('#### Or use the Arduino IDE')
    end = next(
        (number for number in range(start + 1, len(lines)) if lines[number].startswith('#')),
        len(lines),
    )
    return '\n'.join(lines[start:end])


SENTENCE = re.compile(r'(?<=[.!?])\s+')
# A sentence that denies the archive a thing, or sends the reader elsewhere for
# it, is the drift this gate exists to stop; a phrase list would only catch the
# wording the last reread already removed.
SECOND_SOURCE = re.compile(
    r'\b(?:no|not|without|never|lacks?|unless|generat\w*|r2s-arduino)\b', re.IGNORECASE
)


def claims_absent(section: str, thing: str) -> list[str]:
    """Every sentence of the IDE block that puts ``thing`` out of the archive's reach."""
    return [
        sentence
        for sentence in SENTENCE.split(section)
        if re.search(thing, sentence, re.IGNORECASE) and SECOND_SOURCE.search(sentence)
    ]


def test_the_readme_ide_route_states_what_the_zip_holds(tmp_path):
    """The README's IDE block and the archive agree, in both directions.

    A reader adds the ZIP and expects the menu it describes. Promising examples
    the archive lacks sends them to a second route, and so does offering one the
    archive ships. The frame limit follows the same rule: the archive either
    carries `src/r2s_config.h` or the block hands the reader a property to type.
    """
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob('ros2serial-arduino-*.zip'))
    names = zipfile.ZipFile(zip_path).namelist()
    section = readme_ide_section()

    ships_examples = any(name.endswith('.ino') and '/examples/' in name for name in names)
    assert ships_examples, 'the release ZIP stages no sketch'
    assert ('File > Examples > ros2serial' in section) is ships_examples
    assert claims_absent(section, r'examples?') == [], (
        'the README puts the shipped examples out of the archive reach'
    )

    # #675 made the archive configure its own build, so the block may only tell
    # a reader nothing needs configuring while the file is really in it.
    carries_limit = 'ros2serial/src/r2s_config.h' in names
    assert carries_limit, 'the release ZIP stages no r2s_config.h'
    assert claims_absent(section, r'frame[ -]limit') == [], (
        'the README asks the IDE build for a limit the archive already carries'
    )
    for property_name in ('platform.local.txt', 'compiler.c.extra_flags'):
        assert property_name not in section, (
            f'the README asks for {property_name} though the archive carries src/r2s_config.h'
        )


def _plain_compile(library, sketch, board, work):
    return subprocess.run(
        [
            'arduino-cli',
            'compile',
            '--fqbn',
            f'arduino:avr:{board}',
            '--library',
            str(library),
            '--output-dir',
            str(work / f'{sketch}-{board}'),
            str(library / 'examples' / sketch),
        ],
        capture_output=True,
        text=True,
        timeout=600,
    )


@pytest.mark.skipif(shutil.which('arduino-cli') is None, reason='arduino-cli is not installed')
def test_the_shipped_examples_compile_with_no_build_properties(tmp_path):
    """The IDE route: the library alone configures the build (#670)."""
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob('ros2serial-arduino-*.zip'))
    staged = tmp_path / 'staged'
    with zipfile.ZipFile(zip_path) as archive:
        archive.extractall(staged)
    library = staged / 'ros2serial'
    work = tmp_path / 'work'
    work.mkdir()

    for sketch in ('hello', 'button_led'):
        for board in ('uno', 'mega'):
            compiled = _plain_compile(library, sketch, board, work)
            assert compiled.returncode == 0, compiled.stdout + compiled.stderr


def test_an_unknown_distro_fails_loudly(tmp_path):
    out, result = build(tmp_path, '--distro', 'humble')
    assert result.returncode == 1
    assert 'unknown distro' in result.stdout + result.stderr


def test_a_missing_option_value_is_usage(tmp_path):
    result = subprocess.run(['bash', str(SCRIPT), '--out'], capture_output=True, text=True)
    assert result.returncode == 2 and 'usage' in result.stdout


def test_the_python_build_leaves_the_tree_alone(tmp_path):
    """The sdist and wheel build from a throwaway copy, so no staging lands in gen/."""
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert sorted(out.glob('r2s_gen-*.tar.gz')) and sorted(out.glob('r2s_gen-*.whl'))
    assert not (ROOT / 'gen' / 'cpp').exists()


def test_the_sdist_carries_the_cpp_tree_inside_its_staging_dir(tmp_path):
    """The sdist holds the C++ layer inside its own root and leaves the tree alone."""
    work, tarball = sdist_from_clean_copy(tmp_path)
    assert not (work / 'gen' / 'cpp').exists()
    with tarfile.open(tarball) as archive:
        names = archive.getnames()
    assert not any('..' in entry for entry in names)
    for name in (
        'include/r2s/runtime.hpp',
        'include/r2s_bundled.h',
        'src/runtime.cpp',
    ):
        assert any(entry.endswith(name) for entry in names), name


def test_installing_the_sdist_ships_the_cpp_tree(tmp_path):
    """An install from the sdist hands the C++ layer to a build without a checkout."""
    _, tarball = sdist_from_clean_copy(tmp_path)
    venv = tmp_path / 'venv'
    created = subprocess.run(
        [sys.executable, '-m', 'venv', '--system-site-packages', str(venv)],
        capture_output=True,
        text=True,
    )
    assert created.returncode == 0, created.stderr
    installed = subprocess.run(
        [
            str(venv / 'bin' / 'pip'),
            'install',
            '--no-index',
            '--no-build-isolation',
            '--disable-pip-version-check',
            str(tarball),
        ],
        capture_output=True,
        text=True,
    )
    assert installed.returncode == 0, installed.stdout + installed.stderr
    site_packages = next((venv / 'lib').glob('python*/site-packages'))
    cpp_tree = site_packages / 'r2s_gen' / 'cpp'
    assert (cpp_tree / 'include' / 'r2s' / 'runtime.hpp').is_file()
    assert (cpp_tree / 'include' / 'r2s_bundled.h').is_file()
    assert (cpp_tree / 'src' / 'runtime.cpp').is_file()
    staged = subprocess.run(
        [str(venv / 'bin' / 'r2s-arduino'), 'library', '--out', str(tmp_path / 'library')],
        cwd=tmp_path,
        capture_output=True,
        text=True,
    )
    assert staged.returncode == 0, staged.stdout + staged.stderr
    assert (tmp_path / 'library/src/session.c').is_file()


def test_release_wheel_contains_firmware_assets(tmp_path):
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (wheel,) = sorted(out.glob('r2s_gen-*.whl'))
    with zipfile.ZipFile(wheel) as archive:
        names = archive.namelist()
    assert 'r2s_gen/firmware/core/src/session.c' in names
    assert 'r2s_gen/firmware/ports/arduino/r2s_arduino_port.cpp' in names
    assert 'r2s_gen/firmware/examples/button_led/button_led.ino' in names
