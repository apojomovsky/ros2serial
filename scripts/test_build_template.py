# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The template port's scripts work from a renamed copy.

`ports/template/build.sh` and `run.sh` locate their sources from their own
directory, so the guide's copy, rename and build sequence produces a port
that builds and tests under its own name. The build also proves the port
defines the contract's symbols instead of only compiling.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / 'ports' / 'template'
COPY = ROOT / 'ports' / 'myboard'

RENAMES = [
    (Path('r2s_template_port.c'), Path('r2s_myboard_port.c')),
    (Path('r2s_template_port.h'), Path('r2s_myboard_port.h')),
    (Path('r2s_template_regs.h'), Path('r2s_myboard_regs.h')),
    (Path('tests') / 'test_template_port.c', Path('tests') / 'test_myboard_port.c'),
]


def run(args, tmp_path, extra_env=None):
    # This interpreter's directory first: the scripts need the same tools
    # the suite runs with.
    path = os.pathsep.join([str(Path(sys.executable).parent), '/usr/bin:/bin:/usr/local/bin'])
    env = {
        'PATH': path,
        'HOME': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'b'),
        'R2S_CACHE_DIR': str(tmp_path / 'cache'),
    }
    if extra_env:
        env.update(extra_env)
    # A hung copy must fail, never hang the suite.
    return subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, timeout=300)


def make_copy():
    """The guide's copy and rename sequence, without the manual edits."""
    assert not COPY.exists(), 'a ports/myboard copy is already in the tree'
    shutil.copytree(TEMPLATE, COPY)
    try:
        for old, new in RENAMES:
            (COPY / old).rename(COPY / new)
        for _, new in RENAMES:
            path = COPY / new
            text = path.read_text(encoding='utf-8')
            text = text.replace('TEMPLATE', 'MYBOARD').replace('template', 'myboard')
            path.write_text(text, encoding='utf-8')
    except BaseException:
        shutil.rmtree(COPY, ignore_errors=True)
        raise


def drop_copy():
    shutil.rmtree(COPY, ignore_errors=True)


def test_the_documented_copy_builds_and_runs(tmp_path):
    make_copy()
    try:
        build = run(['bash', 'ports/myboard/build.sh'], tmp_path)
        assert build.returncode == 0, build.stdout + build.stderr
        assert 'myboard build:' in build.stdout, build.stdout
        test = run(['bash', 'ports/myboard/run.sh'], tmp_path)
        assert test.returncode == 0, test.stdout + test.stderr
        assert 'myboard port: all checks passed' in test.stdout, test.stdout
        assert 'template port:' not in test.stdout, test.stdout
    finally:
        drop_copy()


def test_the_build_rejects_a_port_missing_a_contract_symbol(tmp_path):
    make_copy()
    try:
        port = COPY / 'r2s_myboard_port.c'
        text = port.read_text(encoding='utf-8')
        text = re.sub(r'uint32_t r2s_port_tick\(void\)\n\{\n(?:[^\n]*\n)*?\}', '', text, count=1)
        assert 'r2s_port_tick' not in text
        port.write_text(text, encoding='utf-8')
        build = run(['bash', 'ports/myboard/build.sh'], tmp_path)
        assert build.returncode != 0, build.stdout + build.stderr
        assert 'r2s_port_tick' in build.stdout + build.stderr
    finally:
        drop_copy()
