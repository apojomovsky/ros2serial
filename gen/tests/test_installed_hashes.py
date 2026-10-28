"""Bundled hashes equal the installed type support's, on this distro.

The bridge job runs this file in every supported distro container, so a
snapshot that drifts from a released distro fails there. Skips only when no
supported distro can be told; a named distro with missing descriptions fails.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from r2s_gen import bundle, msgs, typehash


def _prefixes(ament: str | None = None, distro: str | None = None, opt_ros: Path | None = None) -> list[Path]:
    """This distro's install first, then the sourced prefixes.

    The container's own install is authoritative: an overlay from another
    distro must not shadow the type support under test. The arguments default
    to the environment; the unit tests pass explicit directories.
    """
    if ament is None:
        ament = os.environ.get("AMENT_PREFIX_PATH", "")
    if distro is None:
        distro = os.environ.get("ROS_DISTRO", "")
    if opt_ros is None:
        opt_ros = Path("/opt/ros")
    prefixes = [opt_ros / distro] if distro else []
    if not prefixes and opt_ros.is_dir():
        prefixes.extend(sorted(path for path in opt_ros.iterdir() if path.is_dir()))
    seen = set(prefixes)
    for entry in ament.split(":"):
        if entry and Path(entry) not in seen:
            seen.add(Path(entry))
            prefixes.append(Path(entry))
    return prefixes


def _snapshot_distro() -> str | None:
    """The snapshot matching this container, or None when it cannot be told."""
    distro = os.environ.get("ROS_DISTRO", "")
    if distro in bundle.DISTROS:
        return distro
    if Path("/opt/ros").is_dir():
        candidates = sorted(path.name for path in Path("/opt/ros").iterdir() if path.name in bundle.DISTROS)
        if len(candidates) == 1:
            return candidates[0]
    return None


def _installed_hash(prefix: Path, type_name: str) -> str | None:
    """The installed hash_string for `type_name` under `prefix`, if present."""
    package, _, name = type_name.partition("/msg/")
    document = prefix / "share" / package / "msg" / f"{name}.json"
    if not document.is_file():
        return None
    for entry in json.loads(document.read_text(encoding="utf-8"))["type_hashes"]:
        if entry["type_name"] == type_name:
            return entry["hash_string"]
    raise AssertionError(f"{document} has no hash for {type_name}")


def _mismatches(installed: dict[str, str], generated: dict[str, str]) -> list[str]:
    """One `type: installed X != snapshot Y` line per hash that differs."""
    return [
        f"{type_name}: installed {installed[type_name]} != snapshot {generated[type_name]}"
        for type_name in installed
        if installed[type_name] != generated[type_name]
    ]


def _collect_installed(prefixes: list[Path], types: tuple[str, ...] = bundle.BUNDLED_TYPES) -> dict[str, str]:
    """The installed hash per type from the first prefix that carries it."""
    installed: dict[str, str] = {}
    for type_name in types:
        for prefix in prefixes:
            hashed = _installed_hash(prefix, type_name)
            if hashed is not None:
                installed[type_name] = hashed
                break
    return installed


def test_bundled_hashes_match_the_installed_type_support():
    prefixes = _prefixes()
    installed = _collect_installed(prefixes)
    distro = _snapshot_distro()
    if distro is None:
        pytest.skip("no supported ROS distro found: run in a ROS container (the bridge job does)")
    # A container names its distro, so missing descriptions are a failure, not
    # a skip: the proof must not disappear when the install drifts.
    missing = [type_name for type_name in bundle.BUNDLED_TYPES if type_name not in installed]
    assert not missing, f"no installed type description for {missing} under {prefixes}"
    # Snapshot only, so installed definitions cannot leak into the generator
    # side and hide snapshot drift.
    repository = msgs.MessageRepository(distro=distro)
    generated = {type_name: typehash.type_hash(repository, type_name) for type_name in bundle.BUNDLED_TYPES}
    mismatches = _mismatches(installed, generated)
    assert not mismatches, f"hash drift on {distro}: " + "; ".join(mismatches)


def test_a_missing_entry_names_its_type(tmp_path):
    document_dir = tmp_path / "share" / "demo_msgs" / "msg"
    document_dir.mkdir(parents=True)
    (document_dir / "Sample.json").write_text(
        json.dumps({"type_hashes": [{"type_name": "demo_msgs/msg/Other", "hash_string": "RIHS01_0"}]}),
        encoding="utf-8",
    )

    with pytest.raises(AssertionError, match="demo_msgs/msg/Sample"):
        _installed_hash(tmp_path, "demo_msgs/msg/Sample")


def test_a_hash_difference_names_the_type_and_both_hashes():
    installed = {"demo_msgs/msg/Sample": "RIHS01_aaa"}
    generated = {"demo_msgs/msg/Sample": "RIHS01_bbb"}
    assert _mismatches(installed, generated) == ["demo_msgs/msg/Sample: installed RIHS01_aaa != snapshot RIHS01_bbb"]


def test_matching_hashes_report_no_mismatch():
    hashed = {"demo_msgs/msg/Sample": "RIHS01_aaa"}

    assert _mismatches(hashed, dict(hashed)) == []


def _write_hash(root: Path, type_name: str, hashed: str) -> None:
    """A fake installed type description under `root` carrying one hash."""
    package, _, name = type_name.partition("/msg/")
    document_dir = root / "share" / package / "msg"
    document_dir.mkdir(parents=True)
    (document_dir / f"{name}.json").write_text(
        json.dumps({"type_hashes": [{"type_name": type_name, "hash_string": hashed}]}),
        encoding="utf-8",
    )


def test_the_distro_install_comes_before_sourced_prefixes(tmp_path):
    overlay = tmp_path / "overlay"
    _write_hash(overlay, "demo_msgs/msg/Sample", "RIHS01_overlay")
    opt_ros = tmp_path / "opt" / "ros"
    _write_hash(opt_ros / "jazzy", "demo_msgs/msg/Sample", "RIHS01_distro")

    prefixes = _prefixes(ament=str(overlay), distro="jazzy", opt_ros=opt_ros)

    assert prefixes[0] == opt_ros / "jazzy"
    assert _collect_installed(prefixes, ("demo_msgs/msg/Sample",)) == {"demo_msgs/msg/Sample": "RIHS01_distro"}


def test_collection_is_empty_without_any_installed_description(tmp_path):
    assert _collect_installed([tmp_path], ("demo_msgs/msg/Sample",)) == {}
