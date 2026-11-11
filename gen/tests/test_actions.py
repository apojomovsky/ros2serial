# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Action loading: .action files parse to Goal, Result and Feedback messages."""

import subprocess
from pathlib import Path

import pytest
from r2s_gen import bounds
from r2s_gen import cpp
from r2s_gen import descriptors
from r2s_gen import emit
from r2s_gen import msgs
from r2s_gen import typehash

ROOT = Path(__file__).resolve().parents[2]
CORE_INCLUDE = ROOT / 'core' / 'include'


def write_action(root: Path, type_name: str, text: str) -> Path:
    package, _, name = type_name.partition('/action/')
    path = root / package / 'action' / f'{name}.action'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    return path


def test_an_action_loads_its_goal_result_and_feedback(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    goal = repository.load('demo_msgs/action/Spin_Goal')
    result = repository.load('demo_msgs/action/Spin_Result')
    feedback = repository.load('demo_msgs/action/Spin_Feedback')

    assert [field.name for field in goal.fields] == ['order']
    assert [field.name for field in result.fields] == ['sequence']
    assert [field.name for field in feedback.fields] == ['partial_sequence']


def test_an_empty_feedback_section_is_valid(tmp_path):
    root = tmp_path / 'messages'
    write_action(root, 'demo_msgs/action/Spin', 'int32 order\n---\nint32[] sequence\n---\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load('demo_msgs/action/Spin_Feedback').fields == ()


def test_a_missing_separator_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    path = write_action(root, 'demo_msgs/action/Spin', 'int32 order\n---\nint32 result\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin_Goal')

    assert str(caught.value) == (
        f"{path}: expected 2 '---' separators, found 1; write goal, '---', result, '---', feedback"
    )


def test_an_extra_separator_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    path = write_action(
        root, 'demo_msgs/action/Spin', 'int32 a\n---\nint32 b\n---\nint32 c\n---\n'
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin_Goal')

    assert str(caught.value) == (
        f"{path}: expected 2 '---' separators, found 3; write goal, '---', result, '---', feedback"
    )


def test_a_malformed_goal_line_names_the_file_and_line(tmp_path):
    root = tmp_path / 'messages'
    path = write_action(root, 'demo_msgs/action/Spin', 'float32\n---\nint32 b\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin_Goal')

    assert (
        str(caught.value)
        == f"{path}:1: expected a type and a field name; write it as 'float32 value'"
    )


def test_a_malformed_result_line_names_the_file_and_line(tmp_path):
    root = tmp_path / 'messages'
    path = write_action(root, 'demo_msgs/action/Spin', 'int32 a\n---\nfloat32\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin_Result')

    assert (
        str(caught.value)
        == f"{path}:3: expected a type and a field name; write it as 'float32 value'"
    )


def test_a_malformed_feedback_line_names_the_file_and_line(tmp_path):
    root = tmp_path / 'messages'
    path = write_action(root, 'demo_msgs/action/Spin', 'int32 a\n---\nint32 b\n---\nfloat32\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin_Feedback')

    assert (
        str(caught.value)
        == f"{path}:5: expected a type and a field name; write it as 'float32 value'"
    )


def test_a_bare_action_name_names_its_parts(tmp_path):
    root = tmp_path / 'messages'
    write_action(root, 'demo_msgs/action/Spin', 'int32 a\n---\nint32 b\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Spin')

    assert str(caught.value) == (
        'demo_msgs/action/Spin: an action has no message of its own; '
        "name its goal, result or feedback as 'package/action/Name_Goal'"
    )


def test_an_unknown_action_is_an_error():
    repository = msgs.MessageRepository()

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_msgs/action/Missing_Goal')

    assert str(caught.value) == (
        'demo_msgs/action/Missing: no action definition found; add its package to --msg-path '
        'or check the type name'
    )


def test_a_result_loads_first_from_a_fresh_repository(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    assert [field.name for field in repository.load('demo_msgs/action/Spin_Result').fields] == [
        'sequence'
    ]


def test_load_action_caches_the_definition(tmp_path):
    root = tmp_path / 'messages'
    write_action(root, 'demo_msgs/action/Spin', 'int32 a\n---\nint32 b\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    first = repository.load_action('demo_msgs/action/Spin')

    assert repository.load_action('demo_msgs/action/Spin') is first


def test_load_action_rejects_a_message_type():
    repository = msgs.MessageRepository()

    with pytest.raises(msgs.MessageError) as caught:
        repository.load_action('std_msgs/msg/Float32')

    assert str(caught.value) == (
        "std_msgs/msg/Float32: not an action type; name it as 'package/action/Name'"
    )


def test_an_action_loads_from_a_sourced_prefix(tmp_path):
    package_dir = tmp_path / 'prefix' / 'share' / 'demo_msgs' / 'action'
    package_dir.mkdir(parents=True)
    (package_dir / 'Spin.action').write_text(
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n', encoding='utf-8'
    )
    repository = msgs.MessageRepository(ament_prefix_path=[tmp_path / 'prefix'])

    assert [field.name for field in repository.load('demo_msgs/action/Spin_Goal').fields] == [
        'order'
    ]


def test_action_name_helpers():
    assert cpp.action_name('demo_msgs/action/Spin_Goal') == 'demo_msgs/action/Spin'
    assert cpp.action_name('demo_msgs/action/Spin') == 'demo_msgs/action/Spin'
    assert cpp.message_path('demo_msgs/action/Spin') == 'demo_msgs/action/spin.hpp'

    with pytest.raises(ValueError):
        cpp.action_name('std_msgs/msg/Float32')


def test_emit_action_hpp_rejects_an_unresolved_part(tmp_path):
    root = tmp_path / 'messages'
    write_action(root, 'demo_msgs/action/Spin', 'int32 order\n---\nint32 b\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])
    resolved = bounds.resolve(repository, ['demo_msgs/action/Spin_Goal'], bounds={})

    with pytest.raises(ValueError, match='Spin_Result is not resolved'):
        cpp.emit_action_hpp(resolved, 'demo_msgs/action/Spin', repository)


def test_a_user_action_hashes_deterministically(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    assert (
        typehash.type_hash(repository, 'demo_msgs/action/Spin_Goal')
        == 'RIHS01_524828e1f56ca3e950652e2ac3d5020cb6d2c57cdcc03b016134b77632b9d7a2'
    )
    assert (
        typehash.type_hash(repository, 'demo_msgs/action/Spin_Result')
        == 'RIHS01_ee9d73fa2502829dd6b6ffcb6128b3c768d54c26009abf0d85c53364e61246bf'
    )
    assert (
        typehash.type_hash(repository, 'demo_msgs/action/Spin_Feedback')
        == 'RIHS01_78df82938fcb6f13b833df772843bac61ab8fb2a401904c7f09ee258a3b663a4'
    )


def test_a_bundled_action_hashes_match_the_ros_type_support():
    repository = msgs.MessageRepository()

    assert (
        typehash.type_hash(repository, 'tf2_msgs/action/LookupTransform_Goal')
        == 'RIHS01_815efc294ad17c813c6043cdde781ff55810d49da02ed07f9237dcd20e88d498'
    )
    assert (
        typehash.type_hash(repository, 'tf2_msgs/action/LookupTransform_Result')
        == 'RIHS01_061d4423af5352ff5314c8effef8faab133991a78f1b2bcd1fecabad80871225'
    )
    assert (
        typehash.type_hash(repository, 'tf2_msgs/action/LookupTransform_Feedback')
        == 'RIHS01_2da0b02f990f04404ab8c98a2a68cf5bbc773ae747f96911f2d55499cfb51542'
    )


def _lookup_bounds() -> dict:
    return {
        'tf2_msgs/action/LookupTransform_Goal.target_frame': 16,
        'tf2_msgs/action/LookupTransform_Goal.source_frame': 16,
        'tf2_msgs/action/LookupTransform_Goal.fixed_frame': 16,
        'tf2_msgs/action/LookupTransform_Result.error.error_string': 16,
        'std_msgs/msg/Header.frame_id': 16,
        'geometry_msgs/msg/TransformStamped.child_frame_id': 16,
    }


def _lookup_resolved():
    repository = msgs.MessageRepository()
    roots = [
        'tf2_msgs/action/LookupTransform_Goal',
        'tf2_msgs/action/LookupTransform_Result',
        'tf2_msgs/action/LookupTransform_Feedback',
    ]
    resolved = bounds.resolve(repository, roots, bounds=_lookup_bounds(), default_string_bound=16)
    return repository, resolved


def test_a_bundled_action_emits_c_names_and_descriptors():
    repository, resolved = _lookup_resolved()

    assert 'tf2_msgs__action__LookupTransform_Goal' in emit.emit_types(resolved)
    table = descriptors.build(
        resolved,
        [
            'tf2_msgs/action/LookupTransform_Goal',
            'tf2_msgs/action/LookupTransform_Result',
            'tf2_msgs/action/LookupTransform_Feedback',
        ],
    )
    assert table.index_of['tf2_msgs/action/LookupTransform_Goal'] == 2
    assert table.index_of['tf2_msgs/action/LookupTransform_Result'] == 9
    assert table.index_of['tf2_msgs/action/LookupTransform_Feedback'] == 10


def test_a_bundled_action_emits_its_cpp_header_at_the_rclcpp_path():
    repository, resolved = _lookup_resolved()

    assert (
        cpp.message_path('tf2_msgs/action/LookupTransform_Goal')
        == 'tf2_msgs/action/lookup_transform.hpp'
    )
    assert (
        cpp.cpp_type('tf2_msgs/action/LookupTransform_Goal')
        == 'tf2_msgs::action::LookupTransform_Goal'
    )
    header = cpp.emit_action_hpp(resolved, 'tf2_msgs/action/LookupTransform', repository)
    assert 'struct LookupTransform_Goal\n{' in header
    assert 'struct LookupTransform_Result\n{' in header
    assert 'struct LookupTransform_Feedback\n{' in header


def test_a_user_action_emits_its_cpp_header(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])
    roots = [
        'demo_msgs/action/Spin_Goal',
        'demo_msgs/action/Spin_Result',
        'demo_msgs/action/Spin_Feedback',
    ]
    resolved = bounds.resolve(
        repository,
        roots,
        bounds={
            'demo_msgs/action/Spin_Result.sequence': 4,
            'demo_msgs/action/Spin_Feedback.partial_sequence': 4,
        },
    )
    header = cpp.emit_action_hpp(resolved, 'demo_msgs/action/Spin', repository)

    assert cpp.message_path('demo_msgs/action/Spin_Goal') == 'demo_msgs/action/spin.hpp'
    assert 'struct Spin_Goal\n{' in header
    assert 'struct Spin_Result\n{' in header


GOLDEN = Path(__file__).parent / 'golden' / 'actions'


def test_the_bundled_action_header_matches_its_golden():
    repository, resolved = _lookup_resolved()

    assert cpp.emit_action_hpp(resolved, 'tf2_msgs/action/LookupTransform', repository) == (
        GOLDEN / 'lookup_transform.hpp'
    ).read_text(encoding='utf-8')


def test_the_user_action_header_matches_its_golden(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])
    roots = [
        'demo_msgs/action/Spin_Goal',
        'demo_msgs/action/Spin_Result',
        'demo_msgs/action/Spin_Feedback',
    ]
    resolved = bounds.resolve(
        repository,
        roots,
        bounds={
            'demo_msgs/action/Spin_Result.sequence': 4,
            'demo_msgs/action/Spin_Feedback.partial_sequence': 4,
        },
    )

    assert cpp.emit_action_hpp(resolved, 'demo_msgs/action/Spin', repository) == (
        GOLDEN / 'spin.hpp'
    ).read_text(encoding='utf-8')


def test_a_bundled_action_compiles_as_c99(tmp_path):
    _, resolved = _lookup_resolved()
    (tmp_path / 'r2s_generated.h').write_text(emit.emit_types(resolved), encoding='utf-8')
    source = tmp_path / 'main.c'
    source.write_text(
        '#include "r2s_generated.h"\n\nint main(void)\n{\n    return 0;\n}\n', encoding='utf-8'
    )

    compiled = subprocess.run(
        [
            'cc',
            '-std=c99',
            '-pedantic',
            '-Werror',
            '-I',
            str(tmp_path),
            '-c',
            str(source),
            '-o',
            str(tmp_path / 'main.o'),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr


def test_a_user_action_header_compiles_as_cxx(tmp_path):
    root = tmp_path / 'messages'
    write_action(
        root,
        'demo_msgs/action/Spin',
        'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])
    roots = [
        'demo_msgs/action/Spin_Goal',
        'demo_msgs/action/Spin_Result',
        'demo_msgs/action/Spin_Feedback',
    ]
    resolved = bounds.resolve(
        repository,
        roots,
        bounds={
            'demo_msgs/action/Spin_Result.sequence': 4,
            'demo_msgs/action/Spin_Feedback.partial_sequence': 4,
        },
    )
    (tmp_path / 'r2s_bundled.h').write_text(emit.emit_types(resolved), encoding='utf-8')
    (tmp_path / 'spin.hpp').write_text(
        cpp.emit_action_hpp(resolved, 'demo_msgs/action/Spin', repository), encoding='utf-8'
    )
    source = tmp_path / 'check.cpp'
    source.write_text('#include "spin.hpp"\n', encoding='utf-8')

    compiled = subprocess.run(
        [
            'g++',
            '-std=c++11',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-pedantic',
            '-Wconversion',
            '-fsyntax-only',
            '-I',
            str(CORE_INCLUDE),
            '-I',
            str(tmp_path),
            str(source),
        ],
        capture_output=True,
        text=True,
    )

    assert compiled.returncode == 0, compiled.stderr


def test_a_bundled_action_hash_matches_the_installed_action():
    repository = msgs.MessageRepository()

    assert (
        typehash.action_hash(repository, 'tf2_msgs/action/LookupTransform')
        == 'RIHS01_0b8adf6bc0b5958879e3265b41a457e03558fe523890a81252c70eba97a82c5d'
    )
