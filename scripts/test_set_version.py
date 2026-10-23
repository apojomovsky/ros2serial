"""The release version is written into every file that carries it, or the cut fails."""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "set_version.py"


def tree(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "gen" / "r2s_gen").mkdir(parents=True)
    (root / "VERSION").write_text("0.1.0\n")
    (root / "gen" / "r2s_gen" / "version.py").write_text('__version__ = "0.1.0"\nPROTOCOL_VERSION = 1\n')
    (root / "gen" / "package.xml").write_text(
        '<package format="3">\n  <name>r2s_gen</name>\n  <version>0.1.0</version>\n</package>\n'
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


def test_the_protocol_version_is_left_alone(tmp_path):
    root = tree(tmp_path)
    assert set_version(root, "0.2.0").returncode == 0
    assert "PROTOCOL_VERSION = 1" in (root / "gen" / "r2s_gen" / "version.py").read_text()


def test_a_version_that_is_not_semver_is_refused(tmp_path):
    root = tree(tmp_path)
    for version in ("1.2", "v1.2.3", "1.2.3-rc1"):
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
def test_a_write_failure_puts_back_what_was_already_written(tmp_path):
    root = tree(tmp_path)
    (root / "gen" / "r2s_gen" / "version.py").chmod(0o444)
    result = set_version(root, "0.2.0")
    assert result.returncode != 0
    assert (root / "VERSION").read_text() == "0.1.0\n"
    assert '__version__ = "0.1.0"' in (root / "gen" / "r2s_gen" / "version.py").read_text()
    assert "<version>0.1.0</version>" in (root / "gen" / "package.xml").read_text()
