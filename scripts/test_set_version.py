"""The release version is written into every file that carries it, or the cut fails."""

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "set_version.py"

_spec = importlib.util.spec_from_file_location("set_version", SCRIPT)
assert _spec and _spec.loader
set_version_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(set_version_module)


def tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "gen" / "r2s_gen").mkdir(parents=True)
    (root / "bridge" / "r2s_bridge").mkdir(parents=True)
    (root / "bridge" / "r2s_bridge_tests").mkdir(parents=True)
    (root / "VERSION").write_text("0.1.0\n")
    (root / "gen" / "r2s_gen" / "version.py").write_text('__version__ = "0.1.0"\nPROTOCOL_VERSION = 1\n')
    (root / "gen" / "package.xml").write_text(
        '<package format="3">\n  <name>r2s_gen</name>\n  <version>0.1.0</version>\n</package>\n'
    )
    (root / "bridge" / "r2s_bridge" / "package.xml").write_text(
        '<package format="3">\n  <name>r2s_bridge</name>\n  <version>0.1.0</version>\n</package>\n'
    )
    (root / "bridge" / "r2s_bridge_tests" / "package.xml").write_text(
        '<package format="3">\n  <name>r2s_bridge_tests</name>\n  <version>0.1.0</version>\n</package>\n'
    )
    return root


def set_version(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *args, str(root)], capture_output=True, text=True)


def test_every_carrier_gets_the_version(tmp_path):
    root = tree(tmp_path)
    result = set_version(root, "0.2.0")
    assert result.returncode == 0, result.stderr
    assert (root / "VERSION").read_text() == "0.2.0\n"
    assert '__version__ = "0.2.0"' in (root / "gen" / "r2s_gen" / "version.py").read_text()
    assert "<version>0.2.0</version>" in (root / "gen" / "package.xml").read_text()
    assert "<version>0.2.0</version>" in (root / "bridge" / "r2s_bridge" / "package.xml").read_text()
    assert "<version>0.2.0</version>" in (root / "bridge" / "r2s_bridge_tests" / "package.xml").read_text()


def test_the_protocol_version_is_left_alone(tmp_path):
    root = tree(tmp_path)
    assert set_version(root, "0.2.0").returncode == 0
    assert "PROTOCOL_VERSION = 1" in (root / "gen" / "r2s_gen" / "version.py").read_text()


def test_a_version_that_is_not_semver_is_refused(tmp_path):
    root = tree(tmp_path)
    for version in ("1.2", "v1.2.3", "1.2.3-rc1", "01.2.3", "1\u0662.2.3", "1.2.3\u0664"):
        result = set_version(root, version)
        assert result.returncode == 2 and "not a version" in result.stderr, version
    assert (root / "VERSION").read_text() == "0.1.0\n"


def test_a_file_without_exactly_one_place_to_write_is_reported_and_changes_nothing(tmp_path):
    root = tree(tmp_path)
    (root / "gen" / "package.xml").write_text('<package format="3">\n  <name>r2s_gen</name>\n</package>\n')
    result = set_version(root, "0.2.0")
    assert result.returncode != 0 and "no single place" in result.stderr
    assert (root / "VERSION").read_text() == "0.1.0\n"
    assert '__version__ = "0.1.0"' in (root / "gen" / "r2s_gen" / "version.py").read_text()


def test_two_places_to_write_are_reported(tmp_path):
    root = tree(tmp_path)
    manifest = root / "gen" / "package.xml"
    text = manifest.read_text()
    manifest.write_text(text.replace("</package>", "  <version>0.1.0</version>\n</package>"))
    result = set_version(root, "0.2.0")
    assert result.returncode != 0 and "no single place" in result.stderr
    assert (root / "VERSION").read_text() == "0.1.0\n"


@pytest.mark.skipif(os.geteuid() == 0, reason="root ignores file write permissions")
def test_a_write_failure_leaves_every_carrier_alone(tmp_path):
    root = tree(tmp_path)
    package = root / "gen" / "r2s_gen"
    package.chmod(0o555)
    try:
        result = set_version(root, "0.2.0")
    finally:
        package.chmod(0o755)
    assert result.returncode != 0
    assert (root / "VERSION").read_text() == "0.1.0\n"
    assert '__version__ = "0.1.0"' in (root / "gen" / "r2s_gen" / "version.py").read_text()
    assert "<version>0.1.0</version>" in (root / "gen" / "package.xml").read_text()
    assert not list(root.rglob("*.tmp"))


def test_a_replace_failure_puts_back_the_carriers_already_replaced(tmp_path, monkeypatch):
    root = tree(tmp_path)
    real_replace = os.replace
    calls = 0

    def failing_replace(source, target):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise OSError("disk full")
        real_replace(source, target)

    monkeypatch.setattr(set_version_module.os, "replace", failing_replace)
    with pytest.raises(OSError, match="disk full"):
        set_version_module.set_version(root, "0.2.0")
    assert (root / "VERSION").read_text() == "0.1.0\n"
    assert '__version__ = "0.1.0"' in (root / "gen" / "r2s_gen" / "version.py").read_text()
    assert "<version>0.1.0</version>" in (root / "gen" / "package.xml").read_text()
    assert not list(root.rglob("*.tmp"))
