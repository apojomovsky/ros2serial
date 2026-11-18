# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Bounds resolution: every variable-length field gets a concrete bound or an error."""

import pytest
from r2s_gen import bounds
from r2s_gen import msgs
from r2s_gen.docs import BOUNDS_URL
from support import write_message

HEADER_BOUNDS = {'std_msgs/msg/Header.frame_id': 24}
JOINT_STATE_BOUNDS = {
    **HEADER_BOUNDS,
    'sensor_msgs/msg/JointState.name': [4, 12],
    'sensor_msgs/msg/JointState.position': 4,
    'sensor_msgs/msg/JointState.velocity': 4,
    'sensor_msgs/msg/JointState.effort': 4,
}


def repository() -> msgs.MessageRepository:
    return msgs.MessageRepository()


def test_header_resolves_with_the_configured_frame_id_bound():
    resolved = bounds.resolve(repository(), ['std_msgs/msg/Header'], bounds=HEADER_BOUNDS)

    header = resolved['std_msgs/msg/Header']
    assert header.field('frame_id').string_bound == 24
    assert header.field('stamp').type_name == 'builtin_interfaces/msg/Time'
    assert 'builtin_interfaces/msg/Time' in resolved


def test_imu_resolves_with_bounds_for_its_nested_header():
    resolved = bounds.resolve(repository(), ['sensor_msgs/msg/Imu'], bounds=HEADER_BOUNDS)

    imu = resolved['sensor_msgs/msg/Imu']
    assert imu.field('header').type_name == 'std_msgs/msg/Header'
    assert imu.field('orientation_covariance').array_bound == 9
    assert resolved['std_msgs/msg/Header'].field('frame_id').string_bound == 24


def test_joint_state_resolves_with_configured_bounds():
    resolved = bounds.resolve(
        repository(), ['sensor_msgs/msg/JointState'], bounds=JOINT_STATE_BOUNDS
    )

    joint_state = resolved['sensor_msgs/msg/JointState']
    name = joint_state.field('name')
    assert name.is_array
    assert name.array_bound == 4
    assert name.string_bound == 12
    assert joint_state.field('position').array_bound == 4
    assert resolved['std_msgs/msg/Header'].field('frame_id').string_bound == 24


def test_a_fixed_array_is_not_a_sequence():
    resolved = bounds.resolve(repository(), ['sensor_msgs/msg/Imu'], bounds=HEADER_BOUNDS)

    assert resolved['sensor_msgs/msg/Imu'].field('orientation_covariance').is_sequence is False


def test_a_bounded_array_is_a_sequence():
    resolved = bounds.resolve(
        repository(), ['sensor_msgs/msg/JointState'], bounds=JOINT_STATE_BOUNDS
    )

    joint_state = resolved['sensor_msgs/msg/JointState']
    assert joint_state.field('name').is_sequence is True
    assert joint_state.field('position').is_sequence is True


def test_a_bound_in_the_definition_resolves_without_a_configured_entry(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Label', 'string<=12 name\n')

    resolved = bounds.resolve(msgs.MessageRepository(msg_paths=[root]), ['demo_msgs/msg/Label'])

    assert resolved['demo_msgs/msg/Label'].field('name').string_bound == 12


def test_nothing_but_a_configured_entry_bounds_a_sequence_count():
    configured = {
        key: value
        for key, value in JOINT_STATE_BOUNDS.items()
        if key != 'sensor_msgs/msg/JointState.name'
    }

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(repository(), ['sensor_msgs/msg/JointState'], bounds=configured)

    assert str(caught.value) == (
        'sensor_msgs/msg/JointState.name: unbounded sequence; '
        "set bounds['sensor_msgs/msg/JointState.name'] to its element count; "
        'see https://apojomovsky.github.io/ros2serial/message-bounds.html'
    )


def test_an_unbounded_element_string_in_a_sequence_is_an_error_naming_the_field():
    configured = {
        key: value
        for key, value in JOINT_STATE_BOUNDS.items()
        if key != 'sensor_msgs/msg/JointState.name'
    }
    configured['sensor_msgs/msg/JointState.name'] = 4

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(repository(), ['sensor_msgs/msg/JointState'], bounds=configured)

    assert str(caught.value) == (
        'sensor_msgs/msg/JointState.name: unbounded string; '
        "set bounds['sensor_msgs/msg/JointState.name'] to [count, string_bound]; "
        'see https://apojomovsky.github.io/ros2serial/message-bounds.html'
    )


def test_an_unbounded_string_is_an_error_naming_the_field():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(repository(), ['std_msgs/msg/Header'])

    assert str(caught.value) == (
        'std_msgs/msg/Header.frame_id: unbounded string; '
        "set bounds['std_msgs/msg/Header.frame_id']; see https://apojomovsky.github.io/ros2serial/message-bounds.html"
    )


def test_a_bound_on_a_field_without_a_variable_length_is_an_error():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(), ['geometry_msgs/msg/Vector3'], bounds={'geometry_msgs/msg/Vector3.x': 4}
        )

    assert str(caught.value) == (
        'geometry_msgs/msg/Vector3.x: the field has no variable length; '
        'remove the bound; see https://apojomovsky.github.io/ros2serial/message-bounds.html'
    )


def test_a_bound_on_an_already_bounded_field_is_an_error():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(),
            ['sensor_msgs/msg/Imu'],
            bounds={**HEADER_BOUNDS, 'sensor_msgs/msg/Imu.orientation_covariance': 9},
        )

    assert str(caught.value).startswith(
        'sensor_msgs/msg/Imu.orientation_covariance: the field is already bounded in '
    )
    assert str(caught.value).endswith(f'Imu.msg; remove the bound; see {BOUNDS_URL}')


def test_a_list_bound_on_a_sequence_that_is_not_of_strings_is_an_error():
    configured = {**JOINT_STATE_BOUNDS, 'sensor_msgs/msg/JointState.position': [4, 12]}

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(repository(), ['sensor_msgs/msg/JointState'], bounds=configured)

    assert str(caught.value) == (
        'sensor_msgs/msg/JointState.position: a list bound is [count, string_bound], '
        'which needs a sequence of strings; use an integer bound instead; '
        'see https://apojomovsky.github.io/ros2serial/message-bounds.html'
    )


@pytest.mark.parametrize('declaration', ['string[4] names', 'string[<=4] names'])
def test_a_list_bound_resolves_a_declared_string_array(tmp_path, declaration):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Sample', declaration + '\n')

    resolved = bounds.resolve(
        msgs.MessageRepository(msg_paths=[root]),
        ['demo_msgs/msg/Sample'],
        bounds={'demo_msgs/msg/Sample.names': [4, 12]},
    )

    field = resolved['demo_msgs/msg/Sample'].field('names')
    assert field.array_bound == 4
    assert field.string_bound == 12


def test_a_list_bound_whose_count_differs_from_a_declared_bound_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Sample', 'string[<=4] names\n')

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            msgs.MessageRepository(msg_paths=[root]),
            ['demo_msgs/msg/Sample'],
            bounds={'demo_msgs/msg/Sample.names': [3, 12]},
        )

    assert str(caught.value).startswith(
        'demo_msgs/msg/Sample.names: the sequence length is already bounded in '
    )
    assert str(caught.value).endswith(f'Sample.msg; remove the count; see {BOUNDS_URL}')


@pytest.mark.parametrize(
    ('declaration', 'expected'),
    [
        (
            'wstring text',
            'demo_msgs/msg/Wide.text: wstring is not supported; use a bounded string;'
            ' see https://apojomovsky.github.io/ros2serial/message-bounds.html',
        ),
        (
            'wchar letter',
            'demo_msgs/msg/Wide.letter: wchar is not supported; use an 8-bit integer;'
            ' see https://apojomovsky.github.io/ros2serial/message-bounds.html',
        ),
    ],
)
def test_a_wide_field_is_rejected_naming_the_field(tmp_path, declaration, expected):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Wide', declaration + '\n')

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(msgs.MessageRepository(msg_paths=[root]), ['demo_msgs/msg/Wide'])

    assert str(caught.value) == expected


def test_a_wide_field_in_a_nested_message_is_rejected_naming_the_field(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/Wide', 'wstring text\n')
    write_message(root, 'demo_msgs/msg/Outer', 'Wide wide\n')

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(msgs.MessageRepository(msg_paths=[root]), ['demo_msgs/msg/Outer'])

    assert str(caught.value) == (
        'demo_msgs/msg/Wide.text: wstring is not supported; use a bounded string;'
        ' see https://apojomovsky.github.io/ros2serial/message-bounds.html'
    )


def test_a_recursive_message_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    write_message(root, 'demo_msgs/msg/A', 'B b\n')
    write_message(root, 'demo_msgs/msg/B', 'A a\n')

    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(msgs.MessageRepository(msg_paths=[root]), ['demo_msgs/msg/A'])

    assert str(caught.value) == 'demo_msgs/msg/A: recursive message type; break the cycle'


def test_an_unbounded_string_error_points_at_the_bounds_section():
    """The error is where a reader lands first, so it names the page that explains the rule."""
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(repository(), ['std_msgs/msg/Header'])

    assert str(caught.value).endswith(f'see {BOUNDS_URL}')


def test_the_conflict_error_points_at_the_bounds_section():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(),
            ['sensor_msgs/msg/Imu'],
            bounds={**HEADER_BOUNDS, 'sensor_msgs/msg/Imu.orientation_covariance': 9},
        )

    assert str(caught.value).endswith(f'see {BOUNDS_URL}')


def test_a_bad_bound_value_points_at_the_bounds_section():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(), ['std_msgs/msg/Header'], bounds={'std_msgs/msg/Header.frame_id': 'x'}
        )

    assert str(caught.value).endswith(f'see {BOUNDS_URL}')


def test_a_field_with_no_variable_length_points_at_the_bounds_section():
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(), ['geometry_msgs/msg/Vector3'], bounds={'geometry_msgs/msg/Vector3.x': 4}
        )

    assert str(caught.value).endswith(f'see {BOUNDS_URL}')


def test_a_sequence_bound_below_one_is_refused_by_the_resolver():
    """The config rejects this first, so the resolver's own guard needs its own case."""
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(),
            ['sensor_msgs/msg/JointState'],
            bounds={**JOINT_STATE_BOUNDS, 'sensor_msgs/msg/JointState.position': 0},
        )

    assert str(caught.value) == (
        'sensor_msgs/msg/JointState.position: the sequence bound must be at least 1; '
        f'raise it; see {BOUNDS_URL}'
    )


def test_a_string_pair_with_one_element_is_refused_by_the_resolver():
    """A pair is [count, string_bound]; a one-element list is the shape error."""
    with pytest.raises(msgs.MessageError) as caught:
        bounds.resolve(
            repository(),
            ['sensor_msgs/msg/JointState'],
            bounds={**JOINT_STATE_BOUNDS, 'sensor_msgs/msg/JointState.name': [4]},
        )

    assert str(caught.value) == (
        'sensor_msgs/msg/JointState.name: a list bound is [count, string_bound]; '
        f'give the count and the string bound; see {BOUNDS_URL}'
    )
