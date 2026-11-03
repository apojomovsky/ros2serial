"""The rosidl plugin wrapper: generation from `colcon build` in a test package.

`r2s_rosidl` is a thin shell over `r2s-gen generate --layout cmake`: a fixture
package with one custom message plus standard types builds with colcon, and
the generated files exist and are byte-identical to a direct `r2s-gen` run on
the same inputs. A bad `r2s.yaml` fails the build with `r2s-gen`'s message.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "gen"
PLUGIN = PACKAGE / "r2s_rosidl"

GOOD_CONFIG = """\
node: fw_fixture
topics:
  - name: status
    type: fw_fixture/msg/Custom
    direction: pub
  - name: battery
    type: std_msgs/msg/Float32
    direction: sub
"""

EXPLICIT_CONFIG = """\
node: fw_explicit
topics:
  - name: status
    type: fw_fixture/msg/Custom
    direction: sub
  - name: alive
    type: std_msgs/msg/Bool
    direction: pub
"""

CUSTOM_MSG = """\
float32 temperature
int16 rpm
"""

BAD_CONFIG = """\
node: fw_bad
topics:
  - name: broken
    type: nosuch_pkg/msg/Nope
    direction: pub
"""


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def distro() -> str | None:
    """The ROS distro under test, or None when it cannot be told."""
    name = os.environ.get("ROS_DISTRO", "")
    if name:
        return name
    candidates = (
        sorted(path.name for path in Path("/opt/ros").iterdir() if path.is_dir()) if Path("/opt/ros").is_dir() else []
    )
    if len(candidates) == 1:
        return candidates[0]
    return None


def needs_ros() -> str | None:
    if shutil.which("colcon") is None:
        return "colcon is not installed"
    if distro() is None:
        return "no ROS distro can be told"
    return None


def ros_env() -> dict:
    """The environment with the underlay sourced, so ament_cmake resolves."""
    name = distro()
    setup = Path(f"/opt/ros/{name}/setup.bash")
    if "AMENT_PREFIX_PATH" not in os.environ and setup.is_file():
        sourced = run(["bash", "-c", f"source {setup} && env"], cwd="/tmp")
        assert sourced.returncode == 0, sourced.stderr
        env = dict(os.environ)
        for line in sourced.stdout.splitlines():
            key, _, value = line.partition("=")
            if key in ("AMENT_PREFIX_PATH", "CMAKE_PREFIX_PATH", "PATH", "PYTHONPATH", "ROS_DISTRO"):
                env[key] = value
        return env
    return dict(os.environ)


def package_sources(workspace: Path) -> Path:
    """Copy the generator and the plugin into the workspace sources."""
    src = workspace / "src"
    package = src / "r2s_gen"
    shutil.copytree(
        PACKAGE,
        package,
        ignore=shutil.ignore_patterns("__pycache__", "*.egg-info", "build", "r2s_rosidl"),
    )
    plugin = src / "r2s_rosidl"
    shutil.copytree(PLUGIN, plugin, ignore=shutil.ignore_patterns("__pycache__"))
    return src


def write_fixture(
    src: Path, name: str, config: str, with_msg: bool, call: str = "r2s_generate_firmware(CONFIG r2s.yaml)"
) -> Path:
    """Write a firmware package calling r2s_generate_firmware, returning its directory."""
    package = src / name
    package.mkdir(parents=True)
    (package / "r2s.yaml").write_text(config, encoding="utf-8")
    if with_msg:
        (package / "msg").mkdir()
        (package / "msg" / "Custom.msg").write_text(CUSTOM_MSG, encoding="utf-8")
    (package / "package.xml").write_text(
        f"""\
<?xml version="1.0"?>
<package format="3">
  <name>{name}</name>
  <version>0.1.0</version>
  <description>Fixture firmware package for the r2s_rosidl test.</description>
  <maintainer email="apojomovsky@gmail.com">Alexis Pojomovsky</maintainer>
  <license>MIT</license>
  <buildtool_depend>ament_cmake</buildtool_depend>
  <buildtool_depend>r2s_rosidl</buildtool_depend>
  <depend>std_msgs</depend>
  <export>
    <build_type>ament_cmake</build_type>
  </export>
</package>
""",
        encoding="utf-8",
    )
    (package / "CMakeLists.txt").write_text(
        f"""\
cmake_minimum_required(VERSION 3.22)
project({name})
find_package(ament_cmake REQUIRED)
find_package(r2s_rosidl REQUIRED)
{call}
ament_package()
""",
        encoding="utf-8",
    )
    return package


def colcon_build(workspace: Path, *packages: str):
    """Build packages (with their dependencies) in the workspace, returning the result."""
    return run(
        [
            "colcon",
            "--log-base",
            str(workspace / "log"),
            "build",
            "--base-paths",
            str(workspace / "src"),
            "--build-base",
            str(workspace / "build"),
            "--install-base",
            str(workspace / "install"),
            "--packages-up-to",
            *packages,
            "--event-handlers",
            "console_direct+",
        ],
        cwd=workspace,
        env=ros_env(),
    )


def generated_files(out: Path) -> dict[str, bytes]:
    """The three cmake-layout files the wrapper writes under `out`."""
    return {name: (out / name).read_bytes() for name in ("r2s_generated.h", "r2s_generated.c", "CMakeLists.txt")}


def check_wrapped_matches_direct(tmp_path: Path, workspace: Path, fixture: Path, out: Path, msg_path: Path) -> None:
    """The wrapper output exists and is byte-identical to a direct r2s-gen run."""
    try:
        wrapped = generated_files(out)
    except FileNotFoundError as error:
        pytest.fail(f"the wrapper generated nothing: {error}")
    assert all(wrapped.values())

    script = next((workspace / "install" / "r2s_gen" / "lib").glob("python*/site-packages"))
    program = workspace / "install" / "r2s_gen" / "lib" / "r2s_gen" / "r2s-gen"
    config = str(fixture / "r2s.yaml")
    direct_dir = tmp_path / f"direct-{fixture.name}"
    direct = run(
        [
            sys.executable,
            str(program),
            "generate",
            "--config",
            config,
            "--out",
            str(direct_dir),
            "--layout",
            "cmake",
            "--msg-path",
            str(msg_path),
        ],
        env={**ros_env(), "PYTHONPATH": str(script)},
    )
    assert direct.returncode == 0, direct.stdout + direct.stderr
    for name, text in wrapped.items():
        assert (direct_dir / name).read_bytes() == text


@pytest.mark.skipif(needs_ros() is not None, reason=needs_ros() or "no ROS underlay")
def test_colcon_build_generates_firmware_sources(tmp_path):
    workspace = tmp_path / "ws"
    src = package_sources(workspace)
    fixture = write_fixture(src, "fw_fixture", GOOD_CONFIG, with_msg=True)

    built = colcon_build(workspace, "fw_fixture")

    assert built.returncode == 0, built.stdout + built.stderr
    check_wrapped_matches_direct(
        tmp_path,
        workspace,
        fixture,
        workspace / "build" / "fw_fixture" / "r2s_generated",
        src,
    )


@pytest.mark.skipif(needs_ros() is not None, reason=needs_ros() or "no ROS underlay")
def test_colcon_build_honors_explicit_paths(tmp_path):
    workspace = tmp_path / "ws"
    src = package_sources(workspace)
    write_fixture(src, "fw_fixture", GOOD_CONFIG, with_msg=True)
    fixture = write_fixture(
        src,
        "fw_explicit",
        EXPLICIT_CONFIG,
        with_msg=False,
        call="r2s_generate_firmware(CONFIG r2s.yaml MSG_PATHS .. OUTPUT_DIR custom_out)",
    )

    built = colcon_build(workspace, "fw_explicit")

    assert built.returncode == 0, built.stdout + built.stderr
    check_wrapped_matches_direct(
        tmp_path,
        workspace,
        fixture,
        workspace / "build" / "fw_explicit" / "custom_out",
        src,
    )


@pytest.mark.skipif(needs_ros() is not None, reason=needs_ros() or "no ROS underlay")
def test_colcon_build_fails_on_a_bad_config(tmp_path):
    workspace = tmp_path / "ws"
    src = package_sources(workspace)
    write_fixture(src, "fw_bad", BAD_CONFIG, with_msg=False)

    built = colcon_build(workspace, "fw_bad")

    assert built.returncode != 0
    assert "r2s-gen: error:" in built.stdout + built.stderr
