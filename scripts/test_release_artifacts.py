# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The release artifacts: an Arduino library that compiles, and the Python sdist and wheel."""

import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

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
        ignore=shutil.ignore_patterns('__pycache__', '*.egg-info', 'build', 'dist'),
    )
    shutil.copytree(ROOT / 'cpp', work / 'cpp', ignore=shutil.ignore_patterns('__pycache__'))
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
        'ros2serial/src/runtime.cpp',
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
    assert not [
        path
        for path in files
        if path.relative_to(lib).parts[0] != 'src' and path.name != 'library.properties'
    ], 'every staged file but the manifest sits under src/'
    for name in (
        'src/ros2serial.h',
        'src/r2s_bundled.h',
        'src/r2s_bundled.c',
        'src/r2s_arduino_port.cpp',
        'src/r2s_arduino_port.h',
        'src/r2s/r2s_codec.h',
        'src/runtime.cpp',
    ):
        assert (lib / name).is_file(), name
    defines = layouts.build_defines((lib / 'src/r2s_bundled.h').read_text(encoding='utf-8'))
    assert 'R2S_WITH_CODEC' in defines
    for prefix in ('R2S_MAX_FRAME=', 'R2S_MAX_NEST='):
        matches = [define for define in defines if define.startswith(prefix)]
        assert len(matches) == 1, defines
        assert matches[0].removeprefix(prefix).isdigit(), defines


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
