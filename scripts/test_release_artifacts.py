"""The release artifacts: an Arduino library that compiles, and the Python sdist and wheel."""

import shutil
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "build-release-artifacts.sh"


def build(tmp_path: Path, *args: str) -> tuple[Path, subprocess.CompletedProcess]:
    out = tmp_path / "dist"
    try:
        result = subprocess.run(["bash", str(SCRIPT), "--out", str(out), *args], capture_output=True, text=True)
        return out, result
    finally:
        # setup.py sdist materializes the out-of-tree cpp package dir as
        # gen/cpp, so a suite run would leave the tree dirty and the takeoff
        # stamp disengaged. The build outputs already went to the tmp out.
        shutil.rmtree(ROOT / "gen" / "cpp", ignore_errors=True)


def test_the_arduino_zip_stages_the_bundle_and_the_core(tmp_path):
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    (zip_path,) = sorted(out.glob("ros2serial-arduino-*.zip"))
    names = zipfile.ZipFile(zip_path).namelist()
    for name in (
        "ros2serial/library.properties",
        "ros2serial/src/ros2serial.h",
        "ros2serial/src/r2s_bundled.h",
        "ros2serial/src/r2s_bundled.c",
        "ros2serial/src/r2s/r2s_codec.h",
        "ros2serial/src/session.c",
        "ros2serial/src/r2s/runtime.hpp",
        "ros2serial/src/runtime.cpp",
        "ros2serial/src/r2s_arduino_port.cpp",
        "ros2serial/src/std_msgs/msg/float32.hpp",
    ):
        assert name in names, name
    assert not [name for name in names if name.endswith("test_reset.c")]
    assert sorted(out.glob("r2s_gen-*.tar.gz")) and sorted(out.glob("r2s_gen-*.whl"))


def test_an_unknown_distro_fails_loudly(tmp_path):
    out, result = build(tmp_path, "--distro", "humble")
    assert result.returncode == 1
    assert "unknown distro" in result.stdout + result.stderr


def test_a_missing_option_value_is_usage(tmp_path):
    result = subprocess.run(["bash", str(SCRIPT), "--out"], capture_output=True, text=True)
    assert result.returncode == 2 and "usage" in result.stdout


def test_the_python_build_leaves_the_tree_alone(tmp_path):
    """The sdist and wheel build from a throwaway copy, so no staging lands in gen/."""
    out, result = build(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert sorted(out.glob("r2s_gen-*.tar.gz")) and sorted(out.glob("r2s_gen-*.whl"))
    assert not (ROOT / "gen" / "cpp").exists()
