#!/usr/bin/env python3
# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""List every released dependency's rosdep key and fail on keys that do not resolve.

The farm installs every non-ROS dependency through rosdep, so a declared
dependency without a key fails there. This check runs the same resolution
locally first: for each dependency in the three released package.xml files,
it prints the distro package `rosdep resolve` maps it to on every supported
distro, and exits 1 when any key is missing. A dependency naming a package
released from this repository is expected: it resolves once bloom releases
both together, so the listing marks it in-repo instead of resolving it.

Distros and their Ubuntu versions come from ci/versions.env, the same pins
the bridge CI matrix reads, so a new distro is audited without a script
change. Exits 1 with one line per gap, or with the precise tool failure.
"""

import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from dataclasses import field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROSDEP = os.environ.get('R2S_ROSDEP', 'rosdep')

RELEASED = [
    Path('gen/package.xml'),
    Path('gen/r2s_rosidl/package.xml'),
    Path('bridge/r2s_bridge/package.xml'),
]

PIN = re.compile(r'^ROS_([A-Z0-9]+)_IMAGE=(.*)$')
IMAGE = re.compile(r'^ros:([a-z0-9]+)-[a-z0-9-]+@sha256:[0-9a-f]{64}$')


@dataclass
class Row:
    package: str
    dep: str
    kind: str
    resolved: dict[str, str | None] = field(default_factory=dict)

    @property
    def missing(self) -> list[str]:
        return [distro for distro, hit in self.resolved.items() if hit is None]


def _read_package(package_xml: Path) -> tuple[str, list[tuple[str, str]]]:
    """The package name and every (dependency, tag) it declares, in file order."""
    try:
        root = ET.parse(package_xml).getroot()
    except ET.ParseError as err:
        raise ValueError(f'{package_xml}: invalid package.xml: {err}') from err
    deps = []
    for child in root:
        if (
            (child.tag == 'depend' or child.tag.endswith('_depend'))
            and child.text
            and child.text.strip()
        ):
            deps.append((child.text.strip(), child.tag))
    return root.findtext('name') or package_xml.parent.name, deps


def declared_dependencies(package_xml: Path) -> list[tuple[str, str]]:
    """Every (dependency, tag) the package declares, in file order."""
    return _read_package(package_xml)[1]


def supported_distros(root: Path) -> list[tuple[str, str]]:
    """The (distro, ubuntu version) pairs from the pinned bridge images."""
    versions = root / 'ci' / 'versions.env'
    try:
        lines = versions.read_text().splitlines()
    except OSError as err:
        raise ValueError(f'{versions}: cannot read pins: {err}') from err
    distros = []
    for line in lines:
        match = PIN.match(line.strip())
        if not match:
            continue
        distro = match.group(1).lower()
        image = match.group(2)
        pinned = IMAGE.match(image)
        if pinned is None or pinned.group(1) != distro:
            raise ValueError(
                f"{versions}: ROS_{match.group(1)}_IMAGE is not a digest-pinned image: '{image}'"
            )
        tag = image.split('@')[0]
        distros.append((distro, tag.rsplit('-', 1)[1]))
    seen = [distro for distro, _ in distros]
    if len(set(seen)) != len(seen):
        raise ValueError(f'{versions}: a distro is pinned more than once')
    if not distros:
        raise ValueError(f'{versions}: no ROS_<distro>_IMAGE pins the supported distros')
    return distros


def resolve_key(dep: str, distro: str, os_version: str) -> str | None:
    """The installer and distro package for one key, or None when rosdep has no rule."""
    try:
        proc = subprocess.run(
            [ROSDEP, 'resolve', dep, '--rosdistro', distro, '--os', f'ubuntu:{os_version}'],
            capture_output=True,
            text=True,
            env={**os.environ, 'ROS_DISTRO': distro},
        )
    except FileNotFoundError as err:
        raise RuntimeError(f'{ROSDEP} not found: install rosdep to run the audit') from err
    if proc.returncode == 0:
        installer = ''
        packages = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith('#'):
                installer = line[1:]
            elif line:
                packages.append(line)
        return f'{installer}:{",".join(packages)}'
    if 'no rosdep rule for' in proc.stderr or 'No definition of [' in proc.stderr:
        return None
    raise RuntimeError(
        f'{ROSDEP} resolve {dep} failed on {distro}: {proc.stderr.strip().splitlines()[:1]}'
    )


def audit(root: Path, resolve=resolve_key) -> tuple[list[Row], list[Row]]:
    """Every declared dependency with its per-distro resolution, plus the gaps."""
    distros = supported_distros(root)
    names = set()
    parsed = []
    for rel in RELEASED:
        name, deps = _read_package(root / rel)
        names.add(name)
        parsed.append((name, deps))
    rows = []
    for name, deps in parsed:
        for dep, kind in deps:
            if dep in names:
                hits = {distro: 'in-repo' for distro, _ in distros}
            else:
                hits = {d: resolve(dep, d, v) for d, v in distros}  # (distro, ubuntu version)
            rows.append(Row(name, dep, kind, hits))
    return rows, [row for row in rows if row.missing]


def report(rows: list[Row], gaps: list[Row]) -> int:
    """Print the listing and the gaps; return the exit code."""
    distros = sorted({distro for row in rows for distro in row.resolved})
    print(f'rosdep audit: {len({row.package for row in rows})} packages on {", ".join(distros)}')
    for row in rows:
        hits = ' '.join(f'{distro}={row.resolved[distro]}' for distro in distros)
        print(f'{row.package} {row.dep} [{row.kind}] {hits}')
    for row in gaps:
        print(
            f'gap: {row.package} declares {row.dep} [{row.kind}] with no rosdep key on '
            f'{", ".join(row.missing)}'
        )
    print(f'rosdep audit: {len(rows)} dependencies, {len(gaps)} without a key')
    return 1 if gaps else 0


def main() -> int:
    if shutil.which(ROSDEP) is None:
        print(f'rosdep-audit: {ROSDEP} not found: install rosdep to run the audit')
        return 1
    update = subprocess.run([ROSDEP, 'update'], capture_output=True, text=True)
    if update.returncode != 0:
        print(f'rosdep-audit: rosdep update failed: {update.stderr.strip().splitlines()[:1]}')
        return 1
    try:
        rows, gaps = audit(ROOT)
    except (ValueError, RuntimeError) as err:
        print(f'rosdep-audit: {err}')
        return 1
    return report(rows, gaps)


if __name__ == '__main__':
    sys.exit(main())
