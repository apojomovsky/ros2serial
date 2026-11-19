# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The frame limit is a build input, never a silent default.

`R2S_MAX_FRAME` sizes buffers in separately compiled core translation units,
so a build that leaves it out would compile one limit and announce another
(#640). The header refuses such a build and names the flag, and a build that
passes a value compiles at exactly that value: the capacity the handshake
announces follows it. A library ships its own `r2s_config.h` beside its
sources, so the Arduino routes need no flag; a flag on the command line still
wins, which is how an application overrides the shipped limit.
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


def compile_probe(tmp_path, source, *defines, includes=()):
    path = tmp_path / 'probe.c'
    path.write_text(source, encoding='utf-8')
    extra = [item for include in includes for item in ('-I', str(include))]
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
            *extra,
            *defines,
            str(path),
            '-o',
            str(tmp_path / 'probe'),
        ],
        capture_output=True,
        text=True,
    )


def reported(probe):
    done = subprocess.run([str(probe)], capture_output=True, text=True, check=True)
    return done.stdout.split()


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
        # COB-8: the encoded bound of the limit, one byte per byte plus one.
        assert reported(tmp_path / 'probe') == [str(limit), str(limit + limit // 254 + 1)]


def test_a_shipped_config_header_sets_the_limit_without_a_flag(tmp_path):
    """The library route: r2s_config.h rides on the include path, no -D at all."""
    config = tmp_path / 'src'
    config.mkdir()
    (config / 'r2s_config.h').write_text(
        '#ifndef R2S_MAX_FRAME\n#define R2S_MAX_FRAME 200u\n#endif /* R2S_MAX_FRAME */\n',
        encoding='utf-8',
    )

    done = compile_probe(tmp_path, REPORT, includes=(config,))

    assert done.returncode == 0, done.stderr
    assert reported(tmp_path / 'probe') == ['200', str(200 + 200 // 254 + 1)]


def test_a_flag_overrides_the_shipped_config_header(tmp_path):
    """An application that wants another limit passes the flag; it wins."""
    config = tmp_path / 'src'
    config.mkdir()
    (config / 'r2s_config.h').write_text(
        '#ifndef R2S_MAX_FRAME\n#define R2S_MAX_FRAME 200u\n#endif /* R2S_MAX_FRAME */\n',
        encoding='utf-8',
    )

    done = compile_probe(tmp_path, REPORT, '-DR2S_MAX_FRAME=300', includes=(config,))

    assert done.returncode == 0, done.stderr
    assert reported(tmp_path / 'probe') == ['300', str(300 + 300 // 254 + 1)]
