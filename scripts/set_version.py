#!/usr/bin/env python3
"""Write one version into every file of the repository that carries it.

The release cut computes the next version and calls this: VERSION is the
single source, and the generator package and both ROS manifests state the same
number, so a wheel, a colcon build and a release tag never disagree.

Usage: set_version.py <version> [root]
"""

import argparse
import os
import re
import sys
from pathlib import Path

VERSION_FILE = "VERSION"
PACKAGE_VERSION = Path("gen") / "r2s_gen" / "version.py"
GEN_MANIFEST = Path("gen") / "package.xml"
BRIDGE_MANIFEST = Path("bridge") / "r2s_bridge" / "package.xml"

SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
PACKAGE_VERSION_LINE = re.compile(r'^__version__ = ".*"$', re.MULTILINE)
ROS_MANIFEST_TAG = re.compile(r"<version>[^<]*</version>")


def _updated_text(path: Path, pattern: re.Pattern[str], replacement: str) -> str:
    updated, count = pattern.subn(replacement, path.read_text(encoding="utf-8"))
    if count != 1:
        raise SystemExit(f"set_version: {path} has no single place to write the version")
    return updated


def _write_all(updates: dict[Path, str]) -> None:
    """Replace every carrier from a staged copy, and put back the ones already replaced if a later one fails."""
    originals = {path: path.read_text(encoding="utf-8") for path in updates}
    staged = {path: path.with_name(path.name + ".tmp") for path in updates}
    try:
        for path, staged_path in staged.items():
            staged_path.write_text(updates[path], encoding="utf-8")
        replaced: list[Path] = []
        try:
            for path, staged_path in staged.items():
                os.replace(staged_path, path)
                replaced.append(path)
        except OSError:
            for path in replaced:
                path.write_text(originals[path], encoding="utf-8")
            raise
    finally:
        for staged_path in staged.values():
            staged_path.unlink(missing_ok=True)


def set_version(root: Path, version: str) -> None:
    """Write the version into VERSION, the generator package and both ROS manifests."""
    package = _updated_text(root / PACKAGE_VERSION, PACKAGE_VERSION_LINE, f'__version__ = "{version}"')
    gen = _updated_text(root / GEN_MANIFEST, ROS_MANIFEST_TAG, f"<version>{version}</version>")
    bridge = _updated_text(root / BRIDGE_MANIFEST, ROS_MANIFEST_TAG, f"<version>{version}</version>")
    _write_all(
        {
            root / VERSION_FILE: version + "\n",
            root / PACKAGE_VERSION: package,
            root / GEN_MANIFEST: gen,
            root / BRIDGE_MANIFEST: bridge,
        }
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write the release version into every file that carries it.")
    parser.add_argument("version")
    parser.add_argument("root", nargs="?", default=None)
    args = parser.parse_args(argv)
    if not SEMVER.match(args.version):
        print(f"set_version: {args.version} is not a version of the form major.minor.patch", file=sys.stderr)
        return 2
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    set_version(root, args.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
