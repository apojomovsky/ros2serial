# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The custom-interface example: its interface package and generated firmware.

The example carries one message, one service and one action in a ROS
interface package, and a firmware that uses all three through the generated
static API. Its generated pair is pinned here, so a change to the example's
`r2s.yaml` or to the emitters cannot silently move the bytes the CI firmware
build compiles.
"""

from pathlib import Path

from r2s_gen import actions
from r2s_gen import bounds
from r2s_gen import config
from r2s_gen import layouts
from r2s_gen import msgs
from r2s_gen import services

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT / 'examples' / 'custom_interfaces'
FIRMWARE = EXAMPLE / 'custom_firmware'
GOLDEN = Path(__file__).parent / 'golden' / 'custom_firmware'


def _generate():
    """Emit the example firmware's cmake layout from its own sources."""
    config_path = FIRMWARE / 'r2s.yaml'
    loaded = config.load(config_path)
    repository = msgs.MessageRepository(msg_paths=[ROOT / 'examples'], distro=loaded.distro)
    roots = [topic.type_name for topic in loaded.topics]
    roots += services.roots(loaded)
    roots += actions.roots(loaded)
    resolved = bounds.resolve(repository, roots, bounds=dict(loaded.bounds))
    return layouts.generate(loaded, resolved, repository, 'cmake', source=config_path.name)


def test_the_example_interface_package_defines_one_of_each_kind():
    assert (EXAMPLE / 'msg' / 'SensorReading.msg').is_file()
    assert (EXAMPLE / 'srv' / 'Calibrate.srv').is_file()
    assert (EXAMPLE / 'action' / 'Run.action').is_file()


def test_the_example_firmware_matches_the_generator():
    files = _generate()
    for name, text in files.items():
        assert text == (GOLDEN / name).read_text(encoding='utf-8'), name


def test_the_example_firmware_declares_all_three_kinds():
    files = _generate()
    header = files['r2s_generated.h']
    assert 'custom_interfaces__msg__SensorReading' in header
    assert 'custom_interfaces__srv__Calibrate_Response' in header
    assert 'custom_interfaces__action__Run_Result' in header
    assert '#define R2S_SERVICE_COUNT 1u' in header
    assert '#define R2S_ACTION_COUNT 1u' in header


def test_the_example_hashes_are_the_packages_own():
    """The example's types hash from its package, not from a bundled snapshot."""
    loaded = config.load(FIRMWARE / 'r2s.yaml')
    repository = msgs.MessageRepository(msg_paths=[ROOT / 'examples'], distro=loaded.distro)
    # A definition read from the example package resolves; the snapshot has
    # no custom_interfaces, so a fallback would fail instead of hashing.
    assert repository.load('custom_interfaces/msg/SensorReading').path.is_relative_to(EXAMPLE)
