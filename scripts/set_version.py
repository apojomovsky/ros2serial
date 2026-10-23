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


def _rewrite(path: Path, pattern: re.Pattern, replacement: str) -> None:
    text = path.read_text(encoding="utf-8")
    updated, count = pattern.subn(replacement, text)
    if count != 1:
        raise SystemExit(f"set_version: {path} has no single place to write the version")
    path.write_text(updated, encoding="utf-8")


def set_version(root: Path, version: str) -> None:
    """Write the version into VERSION, the generator package and the ROS manifest."""
    (root / VERSION_FILE).write_text(version + "\n", encoding="utf-8")
    _rewrite(root / PACKAGE_VERSION, PACKAGE_VERSION_LINE, f'__version__ = "{version}"')
    _rewrite(root / ROS_MANIFEST, ROS_MANIFEST_TAG, f"<version>{version}</version>")


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
