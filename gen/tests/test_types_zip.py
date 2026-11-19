# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The type library as the archive the Arduino IDE installs.

Add .ZIP Library is the only way an IDE user adds a library, and the IDE passes
no build property, so the archive must carry the runtime the emitted headers
compile against and a `src/r2s_config.h` whose frame limit the type list sizes.
The last proof unpacks the archive and compiles the example sketch against it
with the Arduino CLI, so the IDE path is checked without the IDE.
"""

import re
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest
from r2s_gen import cli
from r2s_gen import types

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / 'examples'
SKETCH = EXAMPLES / 'arduino_xmacro_custom'
TYPE = 'custom_interfaces/msg/SensorReading'

# The runtime a sketch includes but the type list does not name: the core, its
# headers, the C++ layer and the Arduino port, exactly what the release archive
# stages beside the bundle.
RUNTIME = (
    'src/r2s/r2s.h',
    'src/r2s/runtime.hpp',
    'src/session.c',
    'src/runtime.cpp',
    'src/r2s_arduino_port.cpp',
)


@pytest.fixture(autouse=True)
def jazzy(monkeypatch):
    """Resolve jazzy on every test here, whatever distro sourced the shell.

    The farm legs run this suite under each distro with ROS sourced, and the
    digest keys on the distro, so an unpinned command would compare two
    libraries that were never meant to match.
    """
    monkeypatch.delenv('ROS_DISTRO', raising=False)


def planned():
    """The library the type list resolves to, the archive's expected content."""
    return types.plan([types.Entry(TYPE)], msg_paths=[str(EXAMPLES)], distro='jazzy')


def zipped(tmp_path, *extra):
    """Run the command with --zip and return the directory and the archive."""
    out = tmp_path / 'out'
    archive = tmp_path / 'dist' / 'ros2serial-types.zip'
    code = cli.main(
        [
            'types',
            TYPE,
            '--msg-path',
            str(EXAMPLES),
            '--out',
            str(out),
            '--zip',
            str(archive),
            *extra,
        ]
    )
    assert code == 0, code
    return out, archive


def names(archive):
    with zipfile.ZipFile(archive) as opened:
        return opened.namelist()


def unpacked(tmp_path, archive):
    """Extract the archive and return the library directory the IDE installs."""
    staged = tmp_path / 'ide'
    with zipfile.ZipFile(archive) as opened:
        opened.extractall(staged)
    (library,) = [path for path in staged.iterdir() if path.is_dir()]
    return library


def compile_plain(library, out, *properties):
    """Compile the example sketch against the extracted library, no flags by default."""
    arguments = [
        'arduino-cli',
        'compile',
        '--fqbn',
        'arduino:avr:uno',
        '--library',
        str(library),
        '--output-dir',
        str(out),
    ]
    for definition in properties:
        arguments += [
            '--build-property',
            f'compiler.c.extra_flags={definition}',
            '--build-property',
            f'compiler.cpp.extra_flags={definition}',
        ]
    return subprocess.run([*arguments, str(SKETCH)], capture_output=True, text=True, timeout=600)


def test_the_archive_holds_the_library_under_one_folder_named_for_it(tmp_path):
    """The IDE installs the archive's top folder, so it must be the library's name."""
    library = planned()

    _, archive = zipped(tmp_path)

    assert {name.split('/', 1)[0] for name in names(archive)} == {library.name}


def test_the_archive_carries_every_emitted_file_byte_for_byte(tmp_path):
    """The archive adds the runtime; it never rewrites what the directory holds."""
    library = planned()

    out, archive = zipped(tmp_path)

    emitted = {
        path.relative_to(out).as_posix(): path.read_bytes()
        for path in sorted(out.rglob('*'))
        if path.is_file()
    }
    with zipfile.ZipFile(archive) as opened:
        for name, content in emitted.items():
            assert opened.read(f'{library.name}/{name}') == content, name


def test_the_archive_carries_the_runtime_and_no_test_reset(tmp_path):
    """An IDE build reaches the core, the C++ layer and the port from the library."""
    library = planned()

    _, archive = zipped(tmp_path)

    found = set(names(archive))
    for name in RUNTIME:
        assert f'{library.name}/{name}' in found, name
    assert f'{library.name}/src/r2s_config.h' in found
    assert f'{library.name}/src/test_reset.c' not in found


def test_the_archive_sizes_the_frame_from_the_type_list_under_a_guard(tmp_path):
    """No flag reaches an IDE build, and a flag that does still wins."""
    library = planned()

    _, archive = zipped(tmp_path)

    with zipfile.ZipFile(archive) as opened:
        config = opened.read(f'{library.name}/src/r2s_config.h').decode('utf-8')
    assert config == types.emit(library, 'arduino')['src/r2s_config.h']
    assert '#ifndef R2S_MAX_FRAME\n#define R2S_MAX_FRAME ' in config


@pytest.mark.skipif(shutil.which('arduino-cli') is None, reason='arduino-cli is not installed')
def test_the_unpacked_archive_compiles_the_sketch_with_no_build_properties(tmp_path):
    """The IDE route: the archive alone configures the build."""
    _, archive = zipped(tmp_path)
    library = unpacked(tmp_path, archive)

    compiled = compile_plain(library, tmp_path / 'build')

    assert compiled.returncode == 0, compiled.stdout + compiled.stderr


@pytest.mark.skipif(shutil.which('arduino-cli') is None, reason='arduino-cli is not installed')
def test_a_flagged_limit_overrides_the_archived_one(tmp_path):
    """The shipped limit is a default: a smaller flag fails at the row that overflows."""
    _, archive = zipped(tmp_path)
    library = unpacked(tmp_path, archive)

    compiled = compile_plain(library, tmp_path / 'build', '-DR2S_MAX_FRAME=20')

    assert compiled.returncode != 0
    assert 'reading_pub' in compiled.stdout + compiled.stderr


def test_the_archived_frame_limit_follows_the_type_list_bounds(tmp_path):
    """The limit is sized from the types, so a wider bound reaches a wider frame."""
    limits = []
    for bound in ('40', '200'):
        archive = tmp_path / f'bound-{bound}.zip'
        assert (
            cli.main(
                [
                    'types',
                    'std_msgs/msg/String',
                    '--bound',
                    f'std_msgs/msg/String.data={bound}',
                    '--out',
                    str(tmp_path / f'out-{bound}'),
                    '--zip',
                    str(archive),
                ]
            )
            == 0
        )
        with zipfile.ZipFile(archive) as opened:
            (config_name,) = [n for n in opened.namelist() if n.endswith('src/r2s_config.h')]
            config = opened.read(config_name).decode('utf-8')
        limits.append(int(re.search(r'^#define R2S_MAX_FRAME ([0-9]+)u$', config, re.M).group(1)))

    assert limits[0] < limits[1]


def test_a_zip_under_another_layout_fails_naming_it(tmp_path, capsys):
    """An archive is the Arduino IDE's library: another layout has no such route."""
    assert (
        cli.main(
            [
                'types',
                TYPE,
                '--msg-path',
                str(EXAMPLES),
                '--layout',
                'cmake',
                '--out',
                str(tmp_path / 'out'),
                '--zip',
                str(tmp_path / 'types.zip'),
            ]
        )
        == 1
    )
    assert not (tmp_path / 'types.zip').exists()
    assert 'arduino' in capsys.readouterr().err


def expected_runtime(library):
    """The staged paths, read from the asset trees rather than from the archive."""
    names = set()
    for root, prefix in (
        (ROOT / 'core' / 'include' / 'r2s', 'src/r2s'),
        (ROOT / 'cpp' / 'include' / 'r2s', 'src/r2s'),
        (ROOT / 'cpp' / 'include' / 'ros2serial', 'src/ros2serial'),
        (ROOT / 'core' / 'src', 'src'),
        (ROOT / 'cpp' / 'src', 'src'),
        (ROOT / 'ports' / 'arduino', 'src'),
    ):
        names |= {
            f'{library.name}/{prefix}/{path.relative_to(root).as_posix()}'
            for path in root.rglob('*')
            if path.is_file() and path.suffix in ('.c', '.h', '.cpp', '.hpp')
        }
    # A shipped library carries the port's two glue files and none of the test
    # code: the port's stubs and the core's test source stay behind.
    names -= {
        f'{library.name}/src/test_reset.c',
        f'{library.name}/src/tests/test_arduino_port.cpp',
        f'{library.name}/src/tests/stubs/Arduino.cpp',
        f'{library.name}/src/tests/stubs/Arduino.h',
    }
    return names


def test_the_archive_carries_the_emitted_files_and_the_runtime_and_nothing_else(tmp_path):
    """Every entry is one of the two, under one folder: no scratch file travels."""
    library = planned()

    out, archive = zipped(tmp_path)

    emitted = {
        f'{library.name}/{path.relative_to(out).as_posix()}'
        for path in out.rglob('*')
        if path.is_file()
    }
    with zipfile.ZipFile(archive) as opened:
        found = set(opened.namelist())

    runtime = expected_runtime(library)

    assert found == emitted | runtime


def test_the_runtime_entries_are_compressed(tmp_path):
    """The archive is a download the IDE installs: stored bytes cost three times the size."""
    _, archive = zipped(tmp_path)

    with zipfile.ZipFile(archive) as opened:
        methods = {info.compress_type for info in opened.infolist()}

    assert methods == {zipfile.ZIP_DEFLATED}


def test_a_zip_path_under_a_file_fails_naming_the_path(tmp_path, capsys):
    """A destination the filesystem refuses names the archive, not a raw traceback."""
    blocker = tmp_path / 'not-a-directory'
    blocker.write_text('', encoding='utf-8')

    assert (
        cli.main(
            [
                'types',
                TYPE,
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(tmp_path / 'out'),
                '--zip',
                str(blocker / 'types.zip'),
            ]
        )
        == 1
    )
    assert 'types.zip' in capsys.readouterr().err


def test_two_runs_write_the_same_archive(tmp_path):
    """A rebuilt library is a byte-identical artifact, as the release zip is."""
    archives = []
    for run in ('first', 'second'):
        out = tmp_path / run / 'out'
        archive = tmp_path / run / 'types.zip'
        assert (
            cli.main(
                [
                    'types',
                    TYPE,
                    '--msg-path',
                    str(EXAMPLES),
                    '--out',
                    str(out),
                    '--zip',
                    str(archive),
                ]
            )
            == 0
        )
        archives.append(archive.read_bytes())

    assert archives[0] == archives[1]


def test_a_zip_path_that_is_a_directory_fails_naming_it(tmp_path, capsys):
    assert (
        cli.main(
            [
                'types',
                TYPE,
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(tmp_path / 'out'),
                '--zip',
                str(tmp_path),
            ]
        )
        == 1
    )
    assert str(tmp_path) in capsys.readouterr().err
