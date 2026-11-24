# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""bridge-test.sh with R2S_BRIDGE_SANITIZE=1 builds with the sanitizers.

A stub colcon records its invocations and the leak suppressions it saw;
only a build call may appear under --build-only, and it must carry the
sanitize CMake switch.
"""

import os
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

COLCON_STUB = """#!/usr/bin/env bash
echo "$@" >> "$R2S_COLCON_LOG"
echo "LSAN_OPTIONS=$LSAN_OPTIONS" >> "$R2S_COLCON_LOG"
exit 0
"""


def test_sanitize_build_only_passes_the_sanitize_switch(tmp_path):
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
        'R2S_BRIDGE_SANITIZE': '1',
    }
    result = subprocess.run(
        ['bash', 'scripts/bridge-test.sh', '--build-only'],
        cwd=ROOT,
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    text = log.read_text() if log.exists() else ''
    assert '-DR2S_SANITIZE=ON' in text, 'sanitize build did not pass the switch:\n' + text
    assert 'lsan_suppressions.txt' in text, 'sanitize build set no suppressions:\n' + text
    assert (ROOT / 'bridge' / 'lsan_suppressions.txt').exists()
