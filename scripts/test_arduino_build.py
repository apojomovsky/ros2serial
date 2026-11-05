"""The Arduino build helper stages the library and compiles the sketch.

`ports/arduino/build.sh` is the README quickstart's single Arduino build
step: it stages the library the manual copy commands used to assemble and
reads the frame limits out of the bundled header itself, so the reader
never composes a `--build-property` flag string.
"""

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "ports" / "arduino" / "build.sh"


def run(args, cwd, tmp_path, extra_env=None):
    env = {
        "PATH": "/usr/bin:/bin:/usr/local/bin",
        "HOME": str(tmp_path),
        "R2S_BUILD_DIR": str(tmp_path / "b"),
    }
    if extra_env:
        env.update(extra_env)
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)


def test_the_helper_rejects_an_unknown_board(tmp_path):
    result = run(["bash", str(SCRIPT), "--board", "bogus"], ROOT, tmp_path)
    assert result.returncode != 0
    assert "bogus" in result.stderr


def test_the_helper_rejects_an_unknown_option(tmp_path):
    result = run(["bash", str(SCRIPT), "--sketchbogus"], ROOT, tmp_path)
    assert result.returncode != 0


def test_stage_only_stages_the_library_and_reports_the_limits(tmp_path):
    lib = tmp_path / "lib"
    result = run(["bash", str(SCRIPT), "--board", "uno", "--lib", str(lib), "--stage-only"], ROOT, tmp_path)
    assert result.returncode == 0, result.stderr
    for name in (
        "library.properties",
        "src/ros2serial.h",
        "src/r2s_bundled.h",
        "src/r2s_bundled.c",
        "src/r2s_arduino_port.cpp",
        "src/r2s_arduino_port.h",
        "src/runtime.cpp",
        "src/r2s/runtime.hpp",
        "src/r2s/r2s_codec.h",
    ):
        assert (lib / name).is_file(), name
    assert not (lib / "src" / "test_reset.c").exists()
    assert "-DR2S_WITH_CODEC" in result.stdout
    assert "R2S_MAX_FRAME=" in result.stdout
    assert "R2S_MAX_NEST=" in result.stdout


def test_stage_only_reports_a_bad_header_as_an_r2s_gen_error(tmp_path):
    lib = tmp_path / "lib"
    first = run(["bash", str(SCRIPT), "--board", "uno", "--lib", str(lib), "--stage-only"], ROOT, tmp_path)
    assert first.returncode == 0, first.stderr
    header = lib / "src" / "r2s_bundled.h"
    header.write_bytes(header.read_bytes() + b"\xff\n")
    again = run(
        ["bash", str(SCRIPT), "--board", "uno", "--lib", str(lib), "--stage-only", "--no-bundle"],
        ROOT,
        tmp_path,
    )
    assert again.returncode != 0
    assert "r2s-gen: error:" in again.stderr


def test_the_quickstart_leads_with_the_uno_and_names_no_build_property():
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    started = text.index("## Getting started")
    body = text[started:]
    assert "ports/arduino/build.sh" in body
    uno = body.index("Uno")
    posix = body.index("posix")
    assert uno < posix, "the quickstart must reach the Uno before the posix virtual MCU"
    assert "compiler.c.extra_flags" not in body
    assert "build/uno/arduino_minimal.ino.elf" in body
