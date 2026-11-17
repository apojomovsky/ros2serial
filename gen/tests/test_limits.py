# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""
The limits subcommand: the built-in types' capacities and where each came from.

The golden pins the printed table for every distro. Each key of `BUNDLE_BOUNDS`
appears once, with the storage the resolver produced for it, and a field whose
capacity its own definition sets says so instead.
"""

from pathlib import Path

import pytest
from r2s_gen import bounds
from r2s_gen import bundle
from r2s_gen import cli
from r2s_gen import limits
from r2s_gen import msgs
from support import write_message

GOLDEN = Path(__file__).parent / 'golden' / 'limits'


def library_for(root, type_name, configured):
    repository = msgs.MessageRepository(msg_paths=[root])
    return bundle.Library(
        types=(type_name,),
        bounds=dict(configured),
        distro='jazzy',
        resolved=bounds.resolve(repository, [type_name], bounds=dict(configured)),
        repository=repository,
        origin='test',
        name='test',
        digest='0',
        sentence='test',
    )


@pytest.mark.parametrize('distro', bundle.DISTROS)
def test_the_table_matches_its_golden(distro):
    assert limits.table(distro) == (GOLDEN / f'{distro}.txt').read_text(encoding='utf-8')


def test_the_command_prints_the_jazzy_table(capsys):
    assert cli.main(['limits']) == 0

    assert capsys.readouterr().out == limits.table('jazzy')


def test_an_unknown_distro_names_the_supported_ones(capsys):
    assert cli.main(['limits', '--distro', 'humble']) == 1

    assert capsys.readouterr().err == (
        "r2s-gen: error: --distro: unknown distro 'humble'; use one of: jazzy, lyrical, rolling\n"
    )


def test_every_bundle_bounds_entry_appears_with_the_storage_it_produced():
    printed = {
        f'{row.type_name}.{row.field}': (row.kind, row.limit)
        for row in limits.rows(bundle.bundled_library('jazzy'), 'BUNDLE_BOUNDS')
    }

    assert {key: printed[key] for key in bundle.BUNDLE_BOUNDS} == {
        'geometry_msgs/msg/TransformStamped.child_frame_id': ('string', '16'),
        'sensor_msgs/msg/JointState.name': ('string array', '6 x 16'),
        'sensor_msgs/msg/JointState.position': ('sequence', '6'),
        'sensor_msgs/msg/JointState.velocity': ('sequence', '6'),
        'sensor_msgs/msg/JointState.effort': ('sequence', '6'),
        'std_msgs/msg/Header.frame_id': ('string', '16'),
        'std_msgs/msg/String.data': ('string', '16'),
        'std_srvs/srv/Trigger_Response.message': ('string', '16'),
        'tf2_msgs/action/LookupTransform_Goal.fixed_frame': ('string', '16'),
        'tf2_msgs/action/LookupTransform_Goal.source_frame': ('string', '16'),
        'tf2_msgs/action/LookupTransform_Goal.target_frame': ('string', '16'),
        'tf2_msgs/msg/TF2Error.error_string': ('string', '16'),
        'tf2_msgs/msg/TFMessage.transforms': ('sequence', '1'),
    }


def test_a_listed_bound_and_a_definition_bound_are_told_apart(tmp_path):
    root = tmp_path / 'messages'
    write_message(
        root,
        'demo_msgs/msg/Note',
        'string<=12 title\nfloat32[] cells\nfloat32[3] fixed\nstring<=8[2] names\nuint8 flag\n',
    )
    library = library_for(root, 'demo_msgs/msg/Note', {'demo_msgs/msg/Note.cells': 4})

    listed = limits.rows(library, 'bounds')
    printed = {row.field: (row.kind, row.limit, row.source) for row in listed}

    assert printed == {
        'title': ('string', '12', '.msg'),
        'cells': ('sequence', '4', 'bounds'),
        'fixed': ('array', '3', '.msg'),
        'names': ('string array', '2 x 8', '.msg'),
    }
