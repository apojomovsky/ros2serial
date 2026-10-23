#!/usr/bin/env python3
"""Write one version into every file of the repository that carries it.

The release cut computes the next version and calls this: VERSION is the
single source, and the generator package and its ROS manifest state the same
number, so a wheel, a colcon build and a release tag never disagree.

Usage: set_version.py <version> [root]
"""

import argparse
import re
import sys
from pathlib import Path

VERSION_FILE = "VERSION"
PACKAGE_VERSION = Path("gen") / "r2s_gen" / "version.py"
ROS_MANIFEST = Path("gen") / "package.xml"

SEMVER = re.compile(r"\d+\.\d+\.\d+\Z")
PACKAGE_VERSION_LINE = re.compile(r'^__version__ = ".*"$', re.MULTILINE)
ROS_MANIFEST_TAG = re.compile(r"<version>[^<]*</version>")


def _updated_text(path: Path, pattern: re.Pattern[str], replacement: str) -> str:
    updated, count = pattern.subn(replacement, path.read_text(encoding="utf-8"))
    if count != 1:
        raise SystemExit(f"set_version: {path} has no single place to write the version")
    return updated


def set_version(root: Path, version: str) -> None:
    """Write the version into VERSION, the generator package and the ROS manifest."""
    package = _updated_text(root / PACKAGE_VERSION, PACKAGE_VERSION_LINE, f'__version__ = "{version}"')
    manifest = _updated_text(root / ROS_MANIFEST, ROS_MANIFEST_TAG, f"<version>{version}</version>")
    updates = {
        root / VERSION_FILE: version + "\n",
        root / PACKAGE_VERSION: package,
        root / ROS_MANIFEST: manifest,
    }
    originals = {path: path.read_text(encoding="utf-8") for path in updates}
    written: list[Path] = []
    try:
        for path, text in updates.items():
            path.write_text(text, encoding="utf-8")
            written.append(path)
    except OSError:
        for path in written:
            path.write_text(originals[path], encoding="utf-8")
        raise


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
