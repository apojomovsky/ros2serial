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


def planned():
    """The library the type list resolves to, the archive's expected content."""
    return types.plan([types.Entry(TYPE)], msg_paths=[str(EXAMPLES)])


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


def test_two_runs_write_the_same_archive(tmp_path):
    """A rebuilt library is a byte-identical artifact, as the release zip is."""
    _, first = zipped(tmp_path)
    _, second = zipped(tmp_path, '--distro', 'jazzy')

    assert first.read_bytes() == second.read_bytes()


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
