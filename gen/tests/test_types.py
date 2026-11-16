# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The types subcommand: a library for the user's own interface types.

The pinned file set, the banner naming the type list and its digest, bounds
from the flag and from the file producing the same bytes, a bound that
contradicts the `.msg` naming both, the written config re-reading to the same
storage, and the frame checks that stop a build naming the row, in C, in C++
and on the runtime route.
"""

import shutil
import subprocess
from pathlib import Path

import pytest
from r2s_gen import cli
from r2s_gen import types
from r2s_gen.version import __version__
from support import write_message

ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / 'examples'
CORE_INCLUDE = ROOT / 'core' / 'include'

C_FLAGS = (
    '-std=c99',
    '-pedantic',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wconversion',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-Wundef',
)
CXX_FLAGS = (
    '-std=c++11',
    '-pedantic',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wconversion',
    '-Wshadow',
    '-Wundef',
)

SERVICE_AND_ACTION = (
    types.Entry('custom_interfaces/msg/SensorReading'),
    types.Entry(
        'custom_interfaces/srv/Calibrate', {'custom_interfaces/srv/Calibrate_Response.note': 16}
    ),
    types.Entry('custom_interfaces/action/Run', {'custom_interfaces/action/Run_Result.note': 16}),
)


def note_package(tmp_path):
    """A throwaway package with one unbounded string, one fixed array and one sequence."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Note', 'string<=12 title\nstring text\n')
    write_message(root, 'demo_msgs/msg/Fixed', 'uint8[4] ids\n')
    write_message(root, 'demo_msgs/msg/Names', 'string[] names\n')
    return root


def plan(entries, msg_paths=(EXAMPLES,), source='the command line', distro='jazzy', flagged=None):
    return types.plan(
        list(entries),
        distro=distro,
        msg_paths=[str(path) for path in msg_paths],
        source=source,
        flagged=flagged,
    )


def written(out: Path) -> dict[str, str]:
    return {
        path.relative_to(out).as_posix(): path.read_text(encoding='utf-8')
        for path in sorted(out.rglob('*'))
        if path.is_file()
    }


def compile_c(root: Path, source: Path, defines=()):
    return subprocess.run(
        [
            'cc',
            *C_FLAGS,
            '-fsyntax-only',
            '-I',
            str(root),
            '-I',
            str(root / 'src'),
            '-I',
            str(CORE_INCLUDE),
            *defines,
            str(source),
        ],
        capture_output=True,
        text=True,
    )


def compile_cxx(root: Path, source: Path, defines=()):
    return subprocess.run(
        [
            'g++',
            *CXX_FLAGS,
            '-fsyntax-only',
            '-I',
            str(root),
            '-I',
            str(root / 'src'),
            '-I',
            str(CORE_INCLUDE),
            *defines,
            str(source),
        ],
        capture_output=True,
        text=True,
    )


def staged(tmp_path, files):
    """Write the emitted library into a sketch root the compilers can read."""
    root = tmp_path / 'library'
    for name, text in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    # The C++ runtime layer is the repository's, which the Arduino library
    # stages beside its own files; the emitted library never carries it.
    shutil.copytree(ROOT / 'cpp' / 'include' / 'r2s', root / 'src' / 'r2s', dirs_exist_ok=True)
    return root


def test_the_types_file_set_is_pinned():
    files = types.emit(plan(SERVICE_AND_ACTION), 'arduino')

    assert sorted(files) == sorted(types.files_for(plan(SERVICE_AND_ACTION), 'arduino'))
    assert sorted(files) == sorted(
        (
            'library.properties',
            'src/ros2serial.h',
            'src/ros2serial.hpp',
            'src/r2s_bundled.h',
            'src/r2s_bundled.c',
            'src/custom_interfaces/msg/sensor_reading.hpp',
            'src/custom_interfaces/srv/calibrate_request.hpp',
            'src/custom_interfaces/srv/calibrate_response.hpp',
            'src/custom_interfaces/srv/calibrate.hpp',
            'src/custom_interfaces/action/run.hpp',
        )
    )


def test_the_banner_names_the_type_list_and_its_digest():
    library = plan(SERVICE_AND_ACTION)
    files = types.emit(library, 'arduino')

    for name, text in files.items():
        assert library.digest in text, name
    assert (
        f'from types ({len(library.types)} types, config {library.digest})'
        in files['library.properties']
    )
    assert f'name=ros2serial_types_{library.digest}' in files['library.properties']
    assert f'version={__version__}' in files['library.properties']


def test_two_type_lists_never_share_a_library_name():
    first = plan((types.Entry('custom_interfaces/msg/SensorReading'),))
    second = plan(
        (
            types.Entry('custom_interfaces/msg/SensorReading'),
            types.Entry(
                'custom_interfaces/action/Run',
                {'custom_interfaces/action/Run_Result.note': 16},
            ),
        )
    )

    assert first.digest != second.digest
    assert first.name != second.name
    assert first.digest in first.name and second.digest in second.name


def test_bounds_from_the_flag_and_from_the_file_are_byte_identical(tmp_path):
    root = note_package(tmp_path)
    from_flag = tmp_path / 'flag'
    from_file = tmp_path / 'file'
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text('types:\n  - demo_msgs/msg/Note\nbounds:\n  text: 24\n', encoding='utf-8')

    assert (
        cli.main(
            [
                'types',
                'demo_msgs/msg/Note',
                '--msg-path',
                str(root),
                '--out',
                str(from_flag),
                '--bound',
                'demo_msgs/msg/Note.text=24',
            ]
        )
        == 0
    )
    assert (
        cli.main(
            [
                'types',
                '--types-file',
                str(listing),
                '--msg-path',
                str(root),
                '--out',
                str(from_file),
            ]
        )
        == 0
    )

    assert written(from_flag) == written(from_file)


def test_a_bound_on_an_already_bounded_field_names_the_field_and_the_msg(tmp_path, capsys):
    root = note_package(tmp_path)

    code = cli.main(
        [
            'types',
            'demo_msgs/msg/Fixed',
            '--msg-path',
            str(root),
            '--out',
            str(tmp_path / 'out'),
            '--bound',
            'demo_msgs/msg/Fixed.ids=4',
        ]
    )

    err = capsys.readouterr().err
    assert code == 1
    assert 'demo_msgs/msg/Fixed.ids' in err
    assert 'already bounded' in err
    assert str(root / 'demo_msgs/msg/Fixed.msg') in err


def test_the_written_config_re_reads_to_the_same_resolved_storage(tmp_path):
    config = tmp_path / 'r2s-types.yaml'
    out = tmp_path / 'out'

    assert (
        cli.main(
            [
                'types',
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(out),
                '--bound',
                'custom_interfaces/srv/Calibrate_Response.note=24',
                '--bound',
                'custom_interfaces/action/Run_Result.note=24',
                'custom_interfaces/msg/SensorReading',
                'custom_interfaces/srv/Calibrate',
                'custom_interfaces/action/Run',
                '--write-config',
                str(config),
            ]
        )
        == 0
    )

    written_once = plan(types.read_list(config), source=str(config))
    from_command = plan(
        (
            types.Entry('custom_interfaces/msg/SensorReading'),
            types.Entry('custom_interfaces/srv/Calibrate', {'Response.note': 24}),
            types.Entry('custom_interfaces/action/Run', {'Result.note': 24}),
        )
    )

    assert written_once.resolved == from_command.resolved
    assert written_once.bounds == from_command.bounds
    assert types.emit(written_once, 'arduino') == written(out)
    text = config.read_text(encoding='utf-8')
    assert 'custom_interfaces/action/Run' in text
    assert 'custom_interfaces/srv/Calibrate' in text
    assert 'Run_Result.note: 24' in text


def test_a_row_that_does_not_fit_fails_the_build_naming_its_object(tmp_path):
    library = staged(
        tmp_path,
        types.emit(plan((types.Entry('custom_interfaces/msg/SensorReading'),)), 'arduino'),
    )
    sketch = tmp_path / 'row.c'
    sketch.write_text(
        '#define R2S_NODE_NAME "types_check"\n'
        '#define R2S_TOPICS(X) \\\n'
        '    X(Publisher, custom_interfaces__msg__SensorReading, reading_pub, "reading")\n'
        '#include <ros2serial.h>\n',
        encoding='utf-8',
    )

    fits = compile_c(library, sketch)
    small = compile_c(library, sketch, ('-DR2S_MAX_FRAME=20',))

    assert fits.returncode == 0, fits.stderr
    assert small.returncode != 0
    assert 'reading_pub' in small.stderr


def test_a_cxx_row_that_does_not_fit_fails_the_build_naming_its_object(tmp_path):
    library = staged(
        tmp_path,
        types.emit(plan((types.Entry('custom_interfaces/msg/SensorReading'),)), 'arduino'),
    )
    sketch = tmp_path / 'row.cpp'
    sketch.write_text(
        '#define R2S_NODE_NAME "types_check"\n'
        '#define R2S_TOPICS(X) \\\n'
        '    X(Publisher, custom_interfaces::msg::SensorReading, reading_pub, "reading")\n'
        '#include <ros2serial.h>\n',
        encoding='utf-8',
    )

    fits = compile_cxx(library, sketch)
    small = compile_cxx(library, sketch, ('-DR2S_MAX_FRAME=20',))

    assert fits.returncode == 0, fits.stderr
    assert small.returncode != 0
    assert 'reading_pub' in small.stderr


def test_the_runtime_route_fails_the_build_naming_the_type(tmp_path):
    library = staged(
        tmp_path,
        types.emit(plan((types.Entry('custom_interfaces/msg/SensorReading'),)), 'arduino'),
    )
    sketch = tmp_path / 'runtime.cpp'
    sketch.write_text('#include <ros2serial.h>\n', encoding='utf-8')

    fits = compile_cxx(library, sketch)
    small = compile_cxx(library, sketch, ('-DR2S_MAX_FRAME=20',))

    assert fits.returncode == 0, fits.stderr
    assert small.returncode != 0
    assert 'custom_interfaces/msg/SensorReading' in small.stderr


def test_an_unknown_bound_field_names_it(tmp_path, capsys):
    root = note_package(tmp_path)

    code = cli.main(
        [
            'types',
            'demo_msgs/msg/Note',
            '--msg-path',
            str(root),
            '--out',
            str(tmp_path / 'out'),
            '--bound',
            'demo_msgs/msg/Note.nope=4',
        ]
    )

    assert code == 1
    assert 'demo_msgs/msg/Note.nope' in capsys.readouterr().err


def test_a_service_and_an_action_expand_to_their_parts():
    library = plan(SERVICE_AND_ACTION)

    assert 'custom_interfaces/srv/Calibrate_Request' in library.types
    assert 'custom_interfaces/srv/Calibrate_Response' in library.types
    assert 'custom_interfaces/action/Run_Feedback' in library.types
    assert library.services == ('custom_interfaces/srv/Calibrate',)
    assert library.actions == ('custom_interfaces/action/Run',)


def test_the_types_command_writes_the_library(tmp_path, capsys):
    out = tmp_path / 'out'
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text(
        'types:\n'
        '  - custom_interfaces/msg/SensorReading\n'
        '  - {type: custom_interfaces/srv/Calibrate, bounds: {note: 16}}\n'
        '  - {type: custom_interfaces/action/Run, bounds: {note: 16}}\n',
        encoding='utf-8',
    )

    assert (
        cli.main(
            ['types', '--types-file', str(listing), '--msg-path', str(EXAMPLES), '--out', str(out)]
        )
        == 0
    )

    files = written(out)
    assert sorted(files) == sorted(
        types.files_for(plan(SERVICE_AND_ACTION, source=str(listing)), 'arduino')
    )
    assert 'src/ros2serial.h' in capsys.readouterr().out


def test_the_types_command_rejects_an_unknown_layout(tmp_path, capsys):
    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--layout',
                'meson',
                '--out',
                str(tmp_path / 'out'),
            ]
        )
        == 1
    )
    assert "unknown layout 'meson'" in capsys.readouterr().err


def test_the_types_command_is_deterministic(tmp_path):
    first = tmp_path / 'first'
    second = tmp_path / 'second'
    for out in (first, second):
        assert (
            cli.main(
                [
                    'types',
                    'custom_interfaces/msg/SensorReading',
                    '--msg-path',
                    str(EXAMPLES),
                    '--out',
                    str(out),
                ]
            )
            == 0
        )

    assert written(first) == written(second)


@pytest.mark.parametrize('kind', ['arduino', 'platformio', 'cmake'])
def test_each_layout_writes_its_wrapper(kind, tmp_path):
    files = types.files_for(plan(SERVICE_AND_ACTION), kind)

    assert files[0] in ('library.properties', 'library.json', 'CMakeLists.txt')
