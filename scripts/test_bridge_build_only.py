# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""bridge-test.sh --build-only builds and nothing else, even across its re-execs.

The lease and the build lock re-exec the script, so the flag must survive
them: the e2e legs call --build-only and must never run the bridge tests.
A stub colcon records its invocations; only a build call may appear.
"""

import os
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

COLCON_STUB = """#!/usr/bin/env bash
echo "$@" >> "$R2S_COLCON_LOG"
exit 0
"""


def test_build_only_survives_the_lease_and_lock(tmp_path):
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    (bindir / 'colcon').write_text(COLCON_STUB)
    (bindir / 'colcon').chmod(
        stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH | (bindir / 'colcon').stat().st_mode
    )
    log = tmp_path / 'colcon.log'
    env = {
        'PATH': os.pathsep.join([str(bindir), '/usr/bin', '/bin']),
        'HOME': str(tmp_path),
        'R2S_CACHE_DIR': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'build'),
        'R2S_COLCON_LOG': str(log),
        'ROS_DISTRO': 'jazzy',
        'AMENT_PREFIX_PATH': '/stub',
    }
    result = subprocess.run(
        ['bash', 'scripts/bridge-test.sh', '--build-only'],
        cwd=ROOT,
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    calls = log.read_text().splitlines() if log.exists() else []
    assert len(calls) == 1, 'a --build-only run must call colcon once:\n' + '\n'.join(calls)
    verbs = calls[0].split()
    assert 'build' in verbs, 'the one colcon call is not a build:\n' + calls[0]
    assert 'test' not in verbs and 'test-result' not in verbs, (
        'a --build-only run tested:\n' + calls[0]
    )
