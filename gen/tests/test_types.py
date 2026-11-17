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

import json
import shutil
import subprocess
from pathlib import Path

import pytest
from r2s_gen import cli
from r2s_gen import types
from r2s_gen.version import __version__
from support import write_message
from support import write_service

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
                # The distro comes from the flag, so a sourced ROS_DISTRO in CI
                # cannot make the command and the re-read disagree.
                '--distro',
                'jazzy',
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


def test_the_row_check_counts_the_payload_and_the_announce_exactly(tmp_path):
    """Each term is a literal, so the check fires at the exact byte and not one later."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Big', 'uint8[30] cells\n')
    library = staged(
        tmp_path,
        types.emit(plan((types.Entry('demo_msgs/msg/Big'),), msg_paths=(root,)), 'arduino'),
    )
    sketch = tmp_path / 'row.c'
    name = 'demo_msgs/msg/Big'
    topic = 'reading'
    # 12 fixed bytes, then each name with its one-byte length varint, then the
    # frame overhead: the announce term the row check measures.
    announce = 12 + 1 + len(name) + 1 + len(topic) + 4
    assert announce == 42
    sketch.write_text(
        '#define R2S_NODE_NAME "t"\n'
        '#define R2S_TOPICS(X) \\\n'
        f'    X(Publisher, demo_msgs__msg__Big, reading_pub, "{topic}")\n'
        '#include <ros2serial.h>\n',
        encoding='utf-8',
    )

    # The announce is 42 and the payload plus overhead is 34, so the announce
    # is the binding term, and one byte less must fail.
    exact = compile_c(library, sketch, ('-DR2S_MAX_FRAME=42',))
    short = compile_c(library, sketch, ('-DR2S_MAX_FRAME=41',))

    assert exact.returncode == 0, exact.stderr
    assert short.returncode != 0
    assert 'reading_pub' in short.stderr


def test_a_frame_past_the_protocol_limit_is_refused(tmp_path, capsys):
    root = tmp_path / 'messages'
    # 65300 bytes plus the frame overhead passes 65277, the protocol cap (FRM-9).
    write_message(root, 'demo_msgs/msg/Huge', 'uint8[65300] pixels\n')

    code = cli.main(
        ['types', 'demo_msgs/msg/Huge', '--msg-path', str(root), '--out', str(tmp_path / 'out')]
    )

    err = capsys.readouterr().err
    assert code == 1
    assert 'past the protocol limit' in err
    assert '65277' in err


def test_an_action_and_its_bare_parts_never_share_a_library_name():
    """The action header carries an entity the parts do not, so the name must differ."""
    action = plan(SERVICE_AND_ACTION)
    parts = plan(
        (
            types.Entry('custom_interfaces/msg/SensorReading'),
            types.Entry('custom_interfaces/srv/Calibrate', {'Response.note': 16}),
            types.Entry('custom_interfaces/action/Run_Goal'),
            types.Entry('custom_interfaces/action/Run_Result', {'note': 16}),
            types.Entry('custom_interfaces/action/Run_Feedback'),
        )
    )

    assert action.types == parts.types
    assert action.digest != parts.digest
    assert action.name != parts.name


def test_a_part_named_alone_is_refused_naming_its_sibling(tmp_path, capsys):
    code = cli.main(
        [
            'types',
            'custom_interfaces/srv/Calibrate_Request',
            '--msg-path',
            str(EXAMPLES),
            '--out',
            str(tmp_path / 'out'),
        ]
    )

    err = capsys.readouterr().err
    assert code == 1
    assert 'Calibrate_Response' in err
    assert 'name custom_interfaces/srv/Calibrate instead' in err


def test_a_bound_the_file_sets_on_an_already_bounded_field_names_the_msg(tmp_path, capsys):
    root = note_package(tmp_path)
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text(
        'types:\n  - demo_msgs/msg/Fixed\nbounds:\n  demo_msgs/msg/Fixed.ids: 4\n',
        encoding='utf-8',
    )

    code = cli.main(
        [
            'types',
            '--types-file',
            str(listing),
            '--msg-path',
            str(root),
            '--out',
            str(tmp_path / 'out'),
        ]
    )

    err = capsys.readouterr().err
    assert code == 1
    assert 'demo_msgs/msg/Fixed.ids' in err
    assert str(root / 'demo_msgs/msg/Fixed.msg') in err


def test_a_contradictory_bound_is_refused_in_both_sources(tmp_path, capsys):
    root = note_package(tmp_path)
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text(
        'types:\n  - type: demo_msgs/msg/Note\n    bounds:\n      text: 8\n',
        encoding='utf-8',
    )

    assert (
        cli.main(
            [
                'types',
                'demo_msgs/msg/Note',
                '--msg-path',
                str(root),
                '--out',
                str(tmp_path / 'a'),
                '--bound',
                'demo_msgs/msg/Note.text=8',
                '--bound',
                'demo_msgs/msg/Note.text=16',
            ]
        )
        == 1
    )
    assert "'demo_msgs/msg/Note.text' is already bounded to 8" in capsys.readouterr().err
    assert (
        cli.main(
            [
                'types',
                '--types-file',
                str(listing),
                '--msg-path',
                str(root),
                '--out',
                str(tmp_path / 'b'),
                '--bound',
                'demo_msgs/msg/Note.text=16',
            ]
        )
        == 1
    )
    assert 'is set to 8 and to 16' in capsys.readouterr().err


def test_an_empty_list_and_an_unknown_distro_are_refused(tmp_path, capsys):
    assert cli.main(['types', '--out', str(tmp_path / 'a')]) == 1
    assert 'no types' in capsys.readouterr().err
    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--distro',
                'humble',
                '--out',
                str(tmp_path / 'b'),
            ]
        )
        == 1
    )
    assert "unknown distro 'humble'" in capsys.readouterr().err


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


def test_the_runtime_check_measures_the_longest_name_not_the_largest_payload(tmp_path):
    """A long-named small type and a short-named large one: each term needs its own maximum."""
    root = tmp_path / 'messages'
    long_name = 'demo_msgs/msg/' + 'Wide' * 16
    write_message(root, 'demo_msgs/msg/Big', 'uint8[40] cells\n')
    write_message(root, long_name, 'uint8 one\n')
    library = staged(
        tmp_path,
        types.emit(
            plan(
                (types.Entry('demo_msgs/msg/Big'), types.Entry(long_name)),
                msg_paths=(root,),
            ),
            'arduino',
        ),
    )
    sketch = tmp_path / 'runtime.cpp'
    sketch.write_text('#include <ros2serial.h>\n', encoding='utf-8')
    # The payload term needs 40 plus the overhead; the announce of the long
    # name needs 12 + its varint byte + the name + the overhead, which is more.
    payload = 40 + 4
    assert 12 + 1 + len(long_name) + 4 > payload
    announce = 12 + 1 + len(long_name) + 4

    exact = compile_cxx(library, sketch, (f'-DR2S_MAX_FRAME={announce}',))
    short = compile_cxx(library, sketch, (f'-DR2S_MAX_FRAME={payload}',))

    assert exact.returncode == 0, exact.stderr
    assert short.returncode != 0
    assert long_name in short.stderr


def test_the_written_config_pins_the_distro_it_resolved(tmp_path, monkeypatch):
    """A sourced ROS_DISTRO must not make the file and its re-read disagree."""
    monkeypatch.setenv('ROS_DISTRO', 'rolling')
    config = tmp_path / 'r2s-types.yaml'
    out = tmp_path / 'out'

    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(out),
                '--write-config',
                str(config),
            ]
        )
        == 0
    )

    text = config.read_text(encoding='utf-8')
    assert 'distro: rolling' in text
    reread = plan(types.read_list(config), source=str(config), distro='rolling')
    assert types.emit(reread, 'arduino') == written(out)


@pytest.mark.parametrize(
    ('text', 'expected'),
    [
        ('bounds:\n  text: 4\n', "missing required key 'types'"),
        ('types: 3\n', "'types' must be a list"),
        ('types: []\ncolour: red\n', 'unknown key'),
        ('types:\n  - 3\n', 'expected a type or a mapping'),
        ('types:\n  - type: demo_msgs/msg/Note\n    note: 2\n', "unknown key 'note'"),
        ('types:\n  - bounds: {text: 4}\n', "missing required key 'type'"),
        ('types:\n  - demo_msgs/msg/Note\nbounds: 3\n', "'bounds' must be a mapping"),
        ('types:\n  - demo_msgs/msg/Note\nbounds:\n  "": 3\n', 'is not a field path'),
        ('types: [\n  - a\n', 'not valid YAML'),
        ('- a\n', 'the type list must be a mapping'),
    ],
)
def test_a_bad_type_list_is_refused_with_its_key(tmp_path, text, expected):
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text(text, encoding='utf-8')

    with pytest.raises(types.MessageError) as caught:
        types.read_list(listing)

    assert expected in str(caught.value)


def test_an_unreadable_type_list_names_the_path(tmp_path):
    missing = tmp_path / 'nope.yaml'

    with pytest.raises(types.MessageError) as caught:
        types.read_list(missing)

    assert str(missing) in str(caught.value)


@pytest.mark.parametrize(
    ('flag', 'expected'),
    [
        ('no-separator', 'expected TYPE.FIELD=N'),
        ('demo_msgs/msg/Note.text=', 'expected TYPE.FIELD=N'),
        ('demo_msgs/msg/Note.text=many', "'many' is not a bound"),
        ('demo_msgs/msg/Note.text=[4]', 'is not a bound'),
    ],
)
def test_a_bad_bound_flag_is_refused(flag, expected):
    with pytest.raises(types.MessageError) as caught:
        types.parse_bound(flag)

    assert expected in str(caught.value)


def test_a_pair_bound_flag_reads_its_count_and_string():
    assert types.parse_bound('demo_msgs/msg/Note.names=[4, 16]') == (
        'demo_msgs/msg/Note.names',
        [4, 16],
    )


def test_a_part_key_that_names_no_part_is_refused(tmp_path):
    root = tmp_path / 'messages'
    write_service(root, 'demo_srvs/srv/Spin', 'int32 order\n---\nint32 done\n')

    with pytest.raises(types.MessageError) as caught:
        plan((types.Entry('demo_srvs/srv/Spin', {'missing.field': 4}),), msg_paths=(root,))

    assert 'names no part' in str(caught.value)
    assert 'request, response' in str(caught.value)


def test_a_part_key_that_two_parts_share_is_refused(tmp_path):
    root = tmp_path / 'messages'
    write_service(root, 'demo_srvs/srv/Spin', 'int32 value\n---\nint32 value\n')

    with pytest.raises(types.MessageError) as caught:
        plan((types.Entry('demo_srvs/srv/Spin', {'value': 4}),), msg_paths=(root,))

    assert 'is in more than one part' in str(caught.value)


def test_the_flag_only_and_the_file_only_distro_rules(tmp_path, monkeypatch):
    monkeypatch.delenv('ROS_DISTRO', raising=False)
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_text('types: []\ndistro: lyrical\n', encoding='utf-8')

    assert types._distro('rolling', None) == 'rolling'
    assert types._distro(None, str(listing)) == 'lyrical'
    assert types._distro(None, None) == 'jazzy'
    monkeypatch.setenv('ROS_DISTRO', 'rolling')
    assert types._distro(None, None) == 'rolling'


def test_a_platformio_library_writes_its_flags_from_the_header(tmp_path):
    out = tmp_path / 'out'

    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--layout',
                'platformio',
                '--out',
                str(out),
            ]
        )
        == 0
    )

    manifest = json.loads((out / 'library.json').read_text(encoding='utf-8'))
    assert '-DR2S_MAX_FRAME=' in ' '.join(manifest['build']['flags'])
    assert manifest['headers'] == 'ros2serial.h'


def test_a_cmake_library_reads_its_limits_from_the_header(tmp_path):
    out = tmp_path / 'out'

    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--layout',
                'cmake',
                '--out',
                str(out),
            ]
        )
        == 0
    )

    text = (out / 'CMakeLists.txt').read_text(encoding='utf-8')
    assert 'file(STRINGS src/r2s_bundled.h R2S_MAX_FRAME_LINE' in text
    assert 'target_compile_definitions(' in text


def test_the_written_config_omits_a_bounds_block_when_there_is_none(tmp_path):
    config = tmp_path / 'r2s-types.yaml'

    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(tmp_path / 'out'),
                '--write-config',
                str(config),
            ]
        )
        == 0
    )

    text = config.read_text(encoding='utf-8')
    assert '  - custom_interfaces/msg/SensorReading' in text
    assert 'bounds:' not in text


def test_a_list_bound_on_an_unbounded_string_sequence_bounds_both(tmp_path):
    """The `[count, string]` form fills in both lengths when the .msg has neither."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Names', 'string[] names\n')

    library = plan((types.Entry('demo_msgs/msg/Names', {'names': [4, 16]}),), msg_paths=(root,))

    names = library.resolved['demo_msgs/msg/Names'].field('names')
    assert names.array_bound == 4
    assert names.string_bound == 16


def test_a_list_bound_contradicting_the_sequence_names_the_msg(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Names', 'string[<=2] names\n')

    with pytest.raises(types.MessageError) as caught:
        plan((types.Entry('demo_msgs/msg/Names', {'names': [4, 16]}),), msg_paths=(root,))

    assert 'the sequence length is already bounded in' in str(caught.value)
    assert 'Names.msg' in str(caught.value)


def test_a_bound_on_a_bare_string_names_the_msg(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Text', 'string<=8 text\n')

    with pytest.raises(types.MessageError) as caught:
        plan((types.Entry('demo_msgs/msg/Text', {'text': 8}),), msg_paths=(root,))

    assert 'the field is already bounded in' in str(caught.value)
    assert 'Text.msg' in str(caught.value)


def test_a_nested_type_is_read_once_for_its_fields(tmp_path):
    """A type reached twice from the closure is read once, which the bound check relies on."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Inner', 'string text\n')
    write_message(root, 'demo_msgs/msg/Outer', 'Inner first\ndemo_msgs/msg/Inner second\n')

    library = plan(
        (types.Entry('demo_msgs/msg/Outer', {'demo_msgs/msg/Inner.text': 8}),),
        msg_paths=(root,),
    )

    assert library.resolved['demo_msgs/msg/Inner'].field('text').string_bound == 8


def test_the_written_config_lists_a_service_and_a_plain_type(tmp_path):
    config = tmp_path / 'r2s-types.yaml'

    assert (
        cli.main(
            [
                'types',
                '--msg-path',
                str(EXAMPLES),
                '--out',
                str(tmp_path / 'out'),
                '--bound',
                'custom_interfaces/srv/Calibrate_Response.note=24',
                'custom_interfaces/srv/Calibrate',
                'custom_interfaces/msg/SensorReading',
                '--write-config',
                str(config),
            ]
        )
        == 0
    )

    text = config.read_text(encoding='utf-8')
    assert '  - custom_interfaces/srv/Calibrate\n' in text
    assert '  - custom_interfaces/msg/SensorReading\n' in text
    assert '  custom_interfaces/srv/Calibrate_Response.note: 24' in text


def test_a_type_list_that_is_not_utf8_is_refused(tmp_path):
    listing = tmp_path / 'r2s-types.yaml'
    listing.write_bytes(b'types:\n  - \xff\xfe\n')

    with pytest.raises(types.MessageError) as caught:
        types.read_list(listing)

    assert 'save the file as UTF-8' in str(caught.value)


def test_the_sourced_distro_is_reported_when_it_disagrees(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv('ROS_DISTRO', 'rolling')
    monkeypatch.setenv('AMENT_PREFIX_PATH', '/opt/ros/rolling')

    assert (
        cli.main(
            [
                'types',
                'custom_interfaces/msg/SensorReading',
                '--msg-path',
                str(EXAMPLES),
                '--distro',
                'jazzy',
                '--out',
                str(tmp_path / 'out'),
            ]
        )
        == 0
    )

    assert "the sourced ROS distro is 'rolling'" in capsys.readouterr().err


def test_a_list_bound_that_leaves_a_bounded_string_alone_is_refused(tmp_path):
    """A `[count, string]` on a sequence whose element is bounded by the .msg."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Names', 'string<=8[] names\n')

    with pytest.raises(types.MessageError) as caught:
        plan((types.Entry('demo_msgs/msg/Names', {'names': [2, 16]}),), msg_paths=(root,))

    assert 'the field is already bounded in' in str(caught.value)


def test_a_missing_pyyaml_is_reported_as_a_message(monkeypatch):
    import builtins

    real = builtins.__import__

    def refuse(name, *args, **kwargs):
        if name == 'yaml':
            raise ImportError(name)
        return real(name, *args, **kwargs)

    monkeypatch.setattr(builtins, '__import__', refuse)

    with pytest.raises(types.MessageError) as caught:
        types._parse('r2s-types.yaml', 'types: []')

    assert 'pip install pyyaml' in str(caught.value)


def test_a_config_that_cannot_be_written_names_the_path(tmp_path):
    blocked = tmp_path / 'file'
    blocked.write_text('x', encoding='utf-8')

    with pytest.raises(OSError) as caught:
        types.write_config(blocked / 'r2s-types.yaml', plan(SERVICE_AND_ACTION))

    assert 'cannot write' in str(caught.value)


def test_the_written_config_puts_a_message_bound_beside_its_type(tmp_path):
    """A plain type's bound is written in its own entry, and the file re-reads."""
    root = note_package(tmp_path)
    config = tmp_path / 'r2s-types.yaml'

    assert (
        cli.main(
            [
                'types',
                'demo_msgs/msg/Note',
                '--msg-path',
                str(root),
                '--out',
                str(tmp_path / 'out'),
                '--bound',
                'demo_msgs/msg/Note.text=24',
                '--write-config',
                str(config),
            ]
        )
        == 0
    )

    text = config.read_text(encoding='utf-8')
    assert '  - type: demo_msgs/msg/Note\n    bounds:\n      text: 24\n' in text
    assert 'bounds:\n  demo_msgs' not in text
    assert plan(types.read_list(config), msg_paths=(root,)).bounds == {
        'demo_msgs/msg/Note.text': 24
    }


def test_a_message_named_like_a_service_part_is_a_message(tmp_path):
    """A `_Request`-named message has no service header, so the library ships only its own."""
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Setting_Request', 'uint8 value\n')
    library = plan((types.Entry('demo_msgs/msg/Setting_Request'),), msg_paths=(root,))

    assert library.services == ()
    files = types.emit(library, 'arduino')
    assert sorted(files) == sorted(types.files_for(library, 'arduino'))
    assert 'src/demo_msgs/msg/setting_request.hpp' in files


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
