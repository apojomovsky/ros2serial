"""The generator package installs with pip and builds with colcon."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from r2s_gen.version import PROTOCOL_VERSION, __version__

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "gen"


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def copied_package(tmp_path: Path) -> Path:
    """A copy of the package, so a build leaves its artifacts out of the repository."""
    package = tmp_path / "gen"
    shutil.copytree(PACKAGE, package, ignore=shutil.ignore_patterns("__pycache__", "*.egg-info", "build"))
    return package


def clean_env(**overrides: str) -> dict:
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
    env.update(overrides)
    return env


def test_pip_installs_the_generator_into_a_fresh_venv(tmp_path):
    # System site packages carry the pinned setuptools, so the build runs offline.
    venv = tmp_path / "venv"
    created = run([sys.executable, "-m", "venv", "--system-site-packages", str(venv)])
    assert created.returncode == 0, created.stderr
    installed = run(
        [
            str(venv / "bin" / "pip"),
            "install",
            "--no-index",
            "--no-build-isolation",
            "--disable-pip-version-check",
            str(copied_package(tmp_path)),
        ],
        env=clean_env(),
    )
    assert installed.returncode == 0, installed.stdout + installed.stderr
    printed = run([str(venv / "bin" / "r2s-gen"), "version"], env=clean_env())
    assert printed.returncode == 0, printed.stderr
    assert f"r2s-gen {__version__}" in printed.stdout
    assert f"protocol version {PROTOCOL_VERSION}" in printed.stdout


@pytest.mark.skipif(shutil.which("colcon") is None, reason="colcon is not installed")
def test_colcon_builds_the_ament_python_package(tmp_path):
    package = copied_package(tmp_path)
    workspace = tmp_path / "ws"
    workspace.mkdir()
    built = run(
        [
            "colcon",
            "--log-base",
            str(workspace / "log"),
            "build",
            "--base-paths",
            str(package),
            "--build-base",
            str(workspace / "build"),
            "--install-base",
            str(workspace / "install"),
        ],
        cwd=workspace,
        env=clean_env(),
    )
    assert built.returncode == 0, built.stdout + built.stderr
    installed = workspace / "install" / "r2s_gen"
    assert (installed / "bin" / "r2s-gen").is_file()
    assert (installed / "share" / "ament_index" / "resource_index" / "packages" / "r2s_gen").is_file()
    assert (installed / "share" / "r2s_gen" / "package.xml").is_file()
