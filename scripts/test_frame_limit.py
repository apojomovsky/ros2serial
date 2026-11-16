# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The frame limit is a build input, never a silent default.

`R2S_MAX_FRAME` sizes buffers in separately compiled core translation units,
so a build that leaves it out would compile one limit and announce another
(#640). The header refuses such a build and names the flag, and a build that
passes a value compiles at exactly that value: the capacity the handshake
announces follows it.
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE_INCLUDE = ROOT / 'core' / 'include'

# A translation unit that reads the limit the way the core's own sources do.
PROBE = '#include "r2s/r2s_framer.h"\nint main(void) { return R2S_MAX_FRAME; }\n'

# Reads both numbers the handshake carries (CTL-7, FRM-9), so the announced
# capacity is shown to come from the compiled limit.
REPORT = (
    '#include <stdio.h>\n'
    '#include "r2s/r2s_framer.h"\n'
    'int main(void)\n'
    '{\n'
    '  printf("%d %d\\n", (int)R2S_MAX_FRAME, (int)R2S_FRAMER_RX_CAPACITY);\n'
    '  return 0;\n'
    '}\n'
)


def compile_probe(tmp_path, source, *defines):
    path = tmp_path / 'probe.c'
    path.write_text(source, encoding='utf-8')
    return subprocess.run(
        [
            'cc',
            '-std=c99',
            '-pedantic',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-I',
            str(CORE_INCLUDE),
            *defines,
            str(path),
            '-o',
            str(tmp_path / 'probe'),
        ],
        capture_output=True,
        text=True,
    )


def test_a_build_without_the_frame_limit_fails_naming_it(tmp_path):
    done = compile_probe(tmp_path, PROBE)

    assert done.returncode != 0
    assert 'R2S_MAX_FRAME' in done.stderr
    # The message carries the fix, so the reader does not have to find it.
    assert '-DR2S_MAX_FRAME=' in done.stderr


def test_the_compiled_limit_is_the_one_the_handshake_announces(tmp_path):
    for limit in (52, 325):
        done = compile_probe(tmp_path, REPORT, f'-DR2S_MAX_FRAME={limit}')
        assert done.returncode == 0, done.stderr
        reported = subprocess.run(
            [str(tmp_path / 'probe')], capture_output=True, text=True, check=True
        ).stdout.split()
        # COB-8: the encoded bound of the limit, one byte per byte plus one.
        assert reported == [str(limit), str(limit + limit // 254 + 1)]
