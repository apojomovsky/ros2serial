# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""The rosdep audit lists every released dependency and fails on keys rosdep cannot resolve."""

from pathlib import Path

import pytest
import rosdep_audit

ROOT = Path(__file__).resolve().parent.parent

GEN_PKG = """<?xml version="1.0"?>
<package format="3">
  <name>r2s_gen</name>
  <version>0.2.0</version>
  <description>probe</description>
  <maintainer email="t@example.com">t</maintainer>
  <license>MIT</license>
  <buildtool_depend>ament_python</buildtool_depend>
  <test_depend>python3-pytest</test_depend>
  <export><build_type>ament_python</build_type></export>
</package>
"""

ROSIDL_PKG = """<?xml version="1.0"?>
<package format="3">
  <name>r2s_rosidl</name>
  <version>0.2.0</version>
  <description>probe</description>
  <maintainer email="t@example.com">t</maintainer>
  <license>MIT</license>
  <buildtool_depend>ament_cmake</buildtool_depend>
  <buildtool_depend>r2s_gen</buildtool_depend>
  <export><build_type>ament_cmake</build_type></export>
</package>
"""

BRIDGE_PKG = """<?xml version="1.0"?>
<package format="3">
  <name>r2s_bridge</name>
  <version>0.2.0</version>
  <description>probe</description>
  <maintainer email="t@example.com">t</maintainer>
  <license>MIT</license>
  <buildtool_depend>ament_cmake</buildtool_depend>
  <depend>rclcpp</depend>
  <exec_depend>launch_ros</exec_depend>
  <test_depend>yaml-cpp</test_depend>
  <export><build_type>ament_cmake</build_type></export>
</package>
"""

DIGEST = '0' * 64
VERSIONS = f"""PYTHON_VERSION=3.12
ROS_JAZZY_IMAGE=ros:jazzy-ros-base-noble@sha256:{DIGEST}
ROS_LYRICAL_IMAGE=ros:lyrical-ros-base-resolute@sha256:{DIGEST}
"""


def probe_tree(tmp_path: Path) -> Path:
    (tmp_path / 'gen').mkdir()
    (tmp_path / 'gen' / 'package.xml').write_text(GEN_PKG)
    (tmp_path / 'gen' / 'r2s_rosidl').mkdir()
    (tmp_path / 'gen' / 'r2s_rosidl' / 'package.xml').write_text(ROSIDL_PKG)
    (tmp_path / 'bridge' / 'r2s_bridge').mkdir(parents=True)
    (tmp_path / 'bridge' / 'r2s_bridge' / 'package.xml').write_text(BRIDGE_PKG)
    (tmp_path / 'ci').mkdir()
    (tmp_path / 'ci' / 'versions.env').write_text(VERSIONS)
    return tmp_path


def resolve_ok(dep: str, distro: str, os_version: str) -> str | None:
    return f'apt:ros-{distro}-{dep}'


def test_lists_every_declared_dependency_with_its_kind(tmp_path):
    root = probe_tree(tmp_path)
    rows, gaps = rosdep_audit.audit(root, resolve_ok)
    listed = {(row.package, row.dep, row.kind) for row in rows}
    assert ('r2s_gen', 'ament_python', 'buildtool_depend') in listed
    assert ('r2s_gen', 'python3-pytest', 'test_depend') in listed
    assert ('r2s_rosidl', 'r2s_gen', 'buildtool_depend') in listed
    assert ('r2s_bridge', 'rclcpp', 'depend') in listed
    assert ('r2s_bridge', 'launch_ros', 'exec_depend') in listed
    assert ('r2s_bridge', 'yaml-cpp', 'test_depend') in listed
    assert gaps == []


def test_unresolvable_external_dependency_is_a_gap(tmp_path):
    root = probe_tree(tmp_path)

    def resolve(dep: str, distro: str, os_version: str) -> str | None:
        return None if dep == 'yaml-cpp' else f'apt:ros-{distro}-{dep}'

    rows, gaps = rosdep_audit.audit(root, resolve)
    assert [(gap.package, gap.dep) for gap in gaps] == [('r2s_bridge', 'yaml-cpp')]
    assert rosdep_audit.report(rows, gaps) != 0


def test_a_clean_audit_reports_zero(tmp_path, capsys):
    root = probe_tree(tmp_path)
    rows, gaps = rosdep_audit.audit(root, resolve_ok)
    assert rosdep_audit.report(rows, gaps) == 0
    out = capsys.readouterr().out
    assert 'r2s_bridge' in out and 'rclcpp' in out


def test_in_repo_sibling_needs_no_rosdep_key(tmp_path):
    root = probe_tree(tmp_path)

    def resolve(dep: str, distro: str, os_version: str) -> str | None:
        if dep == 'r2s_gen':
            raise AssertionError('in-repo sibling must not reach rosdep')
        return f'apt:ros-{distro}-{dep}'

    rows, gaps = rosdep_audit.audit(root, resolve)
    assert gaps == []
    sibling = next(row for row in rows if row.dep == 'r2s_gen')
    assert sibling.resolved['jazzy'] == 'in-repo'


def test_distros_come_from_the_pinned_images(tmp_path):
    root = probe_tree(tmp_path)
    assert rosdep_audit.supported_distros(root) == [('jazzy', 'noble'), ('lyrical', 'resolute')]


def test_malformed_image_pin_fails_precisely(tmp_path):
    root = probe_tree(tmp_path)
    (root / 'ci' / 'versions.env').write_text('ROS_JAZZY_IMAGE=ros:jazzy\n')
    with pytest.raises(ValueError, match='versions.env'):
        rosdep_audit.supported_distros(root)


def test_released_gen_package_needs_no_ament_python_buildtool():
    # Released ament_python packages declare no buildtool: colcon-ros owns the
    # build type, and rosdep has no key for it, so the farm would fail it.
    deps = rosdep_audit.declared_dependencies(ROOT / 'gen' / 'package.xml')
    assert 'ament_python' not in [dep for dep, _ in deps]
