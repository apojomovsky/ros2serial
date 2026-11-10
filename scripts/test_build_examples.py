# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The example builds read their compiler flags from the bundled header.

Each inline extractor in build-examples.sh reports a bad header as an
r2s-gen error instead of a traceback, so a non-UTF-8 header fails the
build with a precise line.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-examples.sh'


def extractors():
    text = SCRIPT.read_text(encoding='utf-8')
    blocks = [
        block
        for block in re.findall(r"<<'EOF'\n(.*?)\nEOF", text, re.DOTALL)
        if 'build_defines' in block
    ]
    assert blocks, 'build-examples.sh names no flag extractor'
    return blocks


def run_extractor(block, header):
    env = dict(os.environ, PYTHONPATH=str(ROOT / 'gen'))
    return subprocess.run(
        [sys.executable, '-', str(header)], input=block, capture_output=True, text=True, env=env
    )


def test_the_extractors_report_a_valid_header_as_flags(tmp_path):
    header = tmp_path / 'r2s_bundled.h'
    header.write_text('#define R2S_MAX_FRAME 128u\n#define R2S_MAX_NEST 4u\n', encoding='utf-8')

    for block in extractors():
        completed = run_extractor(block, header)

        assert completed.returncode == 0, completed.stderr
        assert '-DR2S_WITH_CODEC' in completed.stdout
        assert 'R2S_MAX_FRAME=128' in completed.stdout


def test_the_extractors_report_a_non_utf8_header_as_an_r2s_gen_error(tmp_path):
    header = tmp_path / 'r2s_bundled.h'
    header.write_bytes(b'#define R2S_MAX_FRAME 128u\n#define R2S_MAX_NEST 4u\n\xff\n')

    for block in extractors():
        completed = run_extractor(block, header)

        assert completed.returncode == 1
        assert 'r2s-gen: error:' in completed.stderr
