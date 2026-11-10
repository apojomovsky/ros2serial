#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Regenerate every version-stamped generated file in the tree.

The emitted banners carry the generator version, so a version bump without
this step leaves the golden files, the example libraries and the posix
sources behind. `make regen` runs this for developers; scripts/release.sh
runs it as part of the cut. Each group mirrors its golden test's inputs, so
a drift between this script and the tests fails the suite.
"""

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'gen'))

from r2s_gen import bounds  # noqa: E402
from r2s_gen import bundle  # noqa: E402
from r2s_gen import config  # noqa: E402
from r2s_gen import cpp  # noqa: E402
from r2s_gen import descriptors  # noqa: E402
from r2s_gen import emit  # noqa: E402
from r2s_gen import layouts  # noqa: E402
from r2s_gen import msgs  # noqa: E402
from r2s_gen import services  # noqa: E402
from r2s_gen import topics  # noqa: E402
from r2s_gen.bounds import ResolvedMessage  # noqa: E402

GOLDEN = ROOT / 'gen' / 'tests' / 'golden'
EXAMPLES = ROOT / 'examples'
POSIX = ROOT / 'ports' / 'posix'
CPP = ROOT / 'cpp'

HEADER_BOUNDS = {'std_msgs/msg/Header.frame_id': 24}
TF_BOUNDS = {
    **HEADER_BOUNDS,
    'geometry_msgs/msg/TransformStamped.child_frame_id': 24,
    'tf2_msgs/msg/TFMessage.transforms': 4,
}
TYPES_CASES = [
    ('std_msgs/msg/Float32', {}, 'float32.h'),
    ('geometry_msgs/msg/Transform', {}, 'transform.h'),
    ('geometry_msgs/msg/TransformStamped', TF_BOUNDS, 'transform_stamped.h'),
    ('geometry_msgs/msg/Twist', {}, 'twist.h'),
    ('std_msgs/msg/Header', HEADER_BOUNDS, 'header.h'),
    ('tf2_msgs/msg/TFMessage', TF_BOUNDS, 'tf_message.h'),
    (
        'sensor_msgs/msg/JointState',
        {
            **HEADER_BOUNDS,
            'sensor_msgs/msg/JointState.name': [4, 12],
            'sensor_msgs/msg/JointState.position': 4,
            'sensor_msgs/msg/JointState.velocity': 4,
            'sensor_msgs/msg/JointState.effort': 4,
        },
        'joint_state.h',
    ),
    ('sensor_msgs/msg/Imu', HEADER_BOUNDS, 'imu.h'),
    ('sensor_msgs/msg/Range', HEADER_BOUNDS, 'range.h'),
]
DESCRIPTOR_ROOTS = ['std_msgs/msg/Float32', 'geometry_msgs/msg/Twist']

TOPICS_TEXT = """\
node: r2s_posix
topics:
  - name: battery
    type: std_msgs/msg/Float32
    direction: pub
  - name: cmd_vel
    type: geometry_msgs/msg/Twist
    direction: sub
  - name: log_text
    type: std_msgs/msg/String
    direction: pub
bounds:
  std_msgs/msg/String.data: 8
"""


def _resolve(loaded: config.Config) -> tuple[msgs.MessageRepository, dict[str, ResolvedMessage]]:
    # The snapshots carry no services, so service definitions resolve from
    # the installed distro, the same source the firmware's distro reads.
    prefixes = [
        Path(entry) for entry in os.environ.get('AMENT_PREFIX_PATH', '').split(os.pathsep) if entry
    ]
    repository = msgs.MessageRepository(ament_prefix_path=prefixes, distro=loaded.distro)
    roots = [topic.type_name for topic in loaded.topics] + services.roots(loaded)
    return repository, bounds.resolve(repository, roots, bounds=dict(loaded.bounds))


def _refresh_types() -> None:
    for type_name, bound_map, golden in TYPES_CASES:
        resolved = bounds.resolve(msgs.MessageRepository(), [type_name], bounds=bound_map)
        (GOLDEN / 'types' / golden).write_text(emit.emit_types(resolved), encoding='utf-8')


ACTION_BOUNDS = {
    'tf2_msgs/action/LookupTransform_Goal.target_frame': 16,
    'tf2_msgs/action/LookupTransform_Goal.source_frame': 16,
    'tf2_msgs/action/LookupTransform_Goal.fixed_frame': 16,
    'tf2_msgs/action/LookupTransform_Result.error.error_string': 16,
    'std_msgs/msg/Header.frame_id': 16,
    'geometry_msgs/msg/TransformStamped.child_frame_id': 16,
}
ACTION_ROOTS = [
    'tf2_msgs/action/LookupTransform_Goal',
    'tf2_msgs/action/LookupTransform_Result',
    'tf2_msgs/action/LookupTransform_Feedback',
]


def _refresh_actions() -> None:
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(
        repository, ACTION_ROOTS, bounds=ACTION_BOUNDS, default_string_bound=16
    )
    actions = GOLDEN / 'actions'
    actions.mkdir(parents=True, exist_ok=True)
    (actions / 'lookup_transform.hpp').write_text(
        cpp.emit_action_hpp(resolved, 'tf2_msgs/action/LookupTransform', repository),
        encoding='utf-8',
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'messages'
        action_dir = root / 'demo_msgs' / 'action'
        action_dir.mkdir(parents=True)
        (action_dir / 'Spin.action').write_text(
            'int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n', encoding='utf-8'
        )
        user_repo = msgs.MessageRepository(msg_paths=[root])
        user_roots = [
            'demo_msgs/action/Spin_Goal',
            'demo_msgs/action/Spin_Result',
            'demo_msgs/action/Spin_Feedback',
        ]
        user_resolved = bounds.resolve(
            user_repo,
            user_roots,
            bounds={
                'demo_msgs/action/Spin_Result.sequence': 4,
                'demo_msgs/action/Spin_Feedback.partial_sequence': 4,
            },
        )
        (actions / 'spin.hpp').write_text(
            cpp.emit_action_hpp(user_resolved, 'demo_msgs/action/Spin', user_repo),
            encoding='utf-8',
        )


def _refresh_descriptors() -> None:
    repository = msgs.MessageRepository()
    resolved = bounds.resolve(repository, DESCRIPTOR_ROOTS, bounds={})
    table = descriptors.build(resolved, DESCRIPTOR_ROOTS)
    (GOLDEN / 'descriptors' / 'float32_twist.c').write_text(
        descriptors.emit_c(table), encoding='utf-8'
    )


def _load_text(text: str) -> config.Config:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'r2s.yaml'
        path.write_text(text, encoding='utf-8')
        return config.load(path)


def _refresh_topics() -> None:
    loaded = _load_text(TOPICS_TEXT)
    repository, resolved = _resolve(loaded)
    header, source = topics.generate(loaded, resolved, repository)
    (GOLDEN / 'topics' / 'posix.h').write_text(header, encoding='utf-8')
    (GOLDEN / 'topics' / 'posix.c').write_text(source, encoding='utf-8')


def _refresh_bundle() -> None:
    for name, text in bundle.bundle().items():
        if name == 'library.properties':
            (GOLDEN / 'bundle' / name).write_text(text, encoding='utf-8')
        elif name in (
            'src/ros2serial.h',
            'src/ros2serial.hpp',
            'src/r2s_bundled.h',
            'src/r2s_bundled.c',
        ):
            (GOLDEN / 'bundle' / Path(name).name).write_text(text, encoding='utf-8')
            if name != 'src/r2s_bundled.c':
                (CPP / 'include' / Path(name).name).write_text(text, encoding='utf-8')
        else:
            target = CPP / 'include' / Path(name).relative_to('src')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding='utf-8')


def _refresh_layouts() -> None:
    config_path = EXAMPLES / 'cmake_minimal' / 'r2s.yaml'
    loaded = config.load(config_path)
    repository, resolved = _resolve(loaded)
    files = layouts.generate(loaded, resolved, repository, 'arduino', source=config_path.name)
    (GOLDEN / 'layouts' / 'library.properties').write_text(
        files['library.properties'], encoding='utf-8'
    )
    pio_files = layouts.generate(
        loaded, resolved, repository, 'platformio', source=config_path.name
    )
    (GOLDEN / 'layouts' / 'library.json').write_text(pio_files['library.json'], encoding='utf-8')


def _refresh_example(name: str, layout: str) -> None:
    loaded = config.load(EXAMPLES / name / 'r2s.yaml')
    repository, resolved = _resolve(loaded)
    for rel, text in layouts.generate(loaded, resolved, repository, layout).items():
        (EXAMPLES / name / 'lib' / rel).write_text(text, encoding='utf-8')


def _refresh_posix() -> None:
    loaded = config.load(POSIX / 'r2s.yaml')
    repository, resolved = _resolve(loaded)
    header, source = topics.generate(loaded, resolved, repository)
    (POSIX / 'src' / 'r2s_generated.h').write_text(header, encoding='utf-8')
    (POSIX / 'src' / 'r2s_generated.c').write_text(source, encoding='utf-8')


def main() -> int:
    _refresh_types()
    _refresh_actions()
    _refresh_descriptors()
    _refresh_topics()
    _refresh_bundle()
    _refresh_layouts()
    _refresh_example('cmake_minimal', 'cmake')
    _refresh_posix()
    return 0


if __name__ == '__main__':
    sys.exit(main())
