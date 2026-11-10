#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""Install the Task suite runner pinned in ci/versions.env.

Downloads the release tarball, checks its sha256 against the pinned hash,
then extracts the single binary. Stdlib only, so the same script serves
the GitHub runner and the bridge containers, which have python3 but no
curl. The Dockerfile cannot copy this script (its .dockerignore excludes
everything), so the image carries the same steps inline.
"""

import hashlib
import os
import platform
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def pins():
    found = {}
    for line in (ROOT / 'ci' / 'versions.env').read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#') and '=' in line:
            key, value = line.split('=', 1)
            found[key.strip()] = value.strip()
    return found['TASK_VERSION'], found['TASK_SHA256']


def main(argv):
    dest = Path(argv[1] if len(argv) > 1 else '/usr/local/bin')
    if platform.machine() not in ('x86_64', 'AMD64'):
        print(
            f'install-task: {platform.machine()} is not supported: the pinned hash covers '
            'the linux amd64 tarball only',
            file=sys.stderr,
        )
        return 1
    version, pinned = pins()
    url = f'https://github.com/go-task/task/releases/download/v{version}/task_linux_amd64.tar.gz'
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, 'task.tar.gz')
        urllib.request.urlretrieve(url, archive)
        digest = hashlib.sha256(Path(archive).read_bytes()).hexdigest()
        if digest != pinned:
            print(
                f'install-task: {url} hashes to {digest}, ci/versions.env pins {pinned}',
                file=sys.stderr,
            )
            return 1
        with tarfile.open(archive, 'r:gz') as tar:
            member = tar.getmember('task')
            member.uid = member.gid = 0
            tar.extract(member, tmp, filter='data')
        binary = Path(tmp) / 'task'
        binary.chmod(0o755)
        target = dest / 'task'
        target.unlink(missing_ok=True)
        binary.rename(target)
    print(f'install-task: task v{version} installed at {dest / "task"}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
