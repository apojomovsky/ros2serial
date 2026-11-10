# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The STM32F103 build gate stages the port glue and builds it.

`ports/stm32/build.sh` is the X2 Blue Pill step: it resolves the pinned
xPack Arm toolchain (from the image, an override, or a verified fetch into
the cache), generates the firmware pair from the shared `ports/common`
config, stages the core with the Blue Pill glue, then links the minimal
firmware. A `--stage-only` run stages the sources and reports the build
flags without invoking the toolchain, so the suite pins the staging
without the downloads.
"""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'ports' / 'stm32' / 'build.sh'
VERSIONS = ROOT / 'ci' / 'versions.env'


def run(args, cwd, tmp_path, extra_env=None):
    # This interpreter's directory first: the generate step needs the
    # same third-party modules the suite runs with.
    path = os.pathsep.join([str(Path(sys.executable).parent), '/usr/bin:/bin:/usr/local/bin'])
    env = {
        'PATH': path,
        'HOME': str(tmp_path),
        'R2S_BUILD_DIR': str(tmp_path / 'b'),
        'R2S_CACHE_DIR': str(tmp_path / 'cache'),
    }
    if extra_env:
        env.update(extra_env)
    # A parse regression must fail, never hang the suite.
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, timeout=300)


def pinned(name):
    for line in VERSIONS.read_text(encoding='utf-8').splitlines():
        if line.startswith(name + '='):
            return line.split('=', 1)[1]
    raise AssertionError(f'ci/versions.env names no {name}')


def test_the_toolchain_version_is_pinned_in_versions_env():
    assert pinned('ARM_GCC_VERSION').count('.') >= 2, pinned('ARM_GCC_VERSION')


def test_the_toolchain_hash_is_pinned_in_versions_env():
    import re

    assert re.fullmatch(r'[0-9a-f]{64}', pinned('ARM_GCC_SHA256')), pinned('ARM_GCC_SHA256')


def test_the_script_reads_both_pins_from_versions_env():
    text = SCRIPT.read_text(encoding='utf-8')
    for name in ('ARM_GCC_VERSION', 'ARM_GCC_SHA256'):
        assert name in text, name
    assert 'ci/versions.env' in text


def test_the_script_builds_the_blue_pill_only():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'bluepill' in text
    assert 'cortex-m3' in text
    assert 'stm32f103.ld' in text
    assert 'pico' not in text.lower()


def test_the_script_stages_the_shared_config_and_the_blue_pill_glue():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'ports/common' in text
    for name in ('firmware.c', 'r2s_board.h', 'r2s.yaml', 'r2s_bluepill.c', 'stm32f103.ld'):
        assert name in text, name


def test_the_toolchain_cache_lands_beside_the_other_bundles():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'R2S_CACHE_DIR' in text
    assert 'xpack' in text
    assert '/opt' not in text


def test_the_helper_rejects_an_unknown_option(tmp_path):
    result = run(['bash', str(SCRIPT), '--sketchbogus'], ROOT, tmp_path)

    assert result.returncode != 0
    assert 'unknown option' in result.stderr


def test_the_helper_rejects_a_missing_option_value_without_hanging(tmp_path):
    result = run(['bash', str(SCRIPT), '--out'], ROOT, tmp_path)

    assert result.returncode != 0
    assert 'needs a value' in result.stderr


def test_the_helper_fails_when_versions_env_names_no_arm_gcc(tmp_path):
    result = run(
        ['bash', str(SCRIPT), '--stage-only'],
        ROOT,
        tmp_path,
        extra_env={'R2S_VERSIONS_ENV': str(tmp_path / 'empty-versions.env')},
    )

    assert result.returncode != 0
    assert 'ARM_GCC_VERSION' in result.stderr


def stub_path(bin_dir):
    """The stub toolchain first, then the suite interpreter for r2s-gen."""
    return os.pathsep.join(
        [str(bin_dir), str(Path(sys.executable).parent), '/usr/bin:/bin:/usr/local/bin']
    )


def stub_arm(bin_dir, log):
    """A stub Arm toolchain: compiles touch their outputs."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    gcc = bin_dir / 'arm-none-eabi-gcc'
    gcc.write_text(
        '#!/usr/bin/env bash\n'
        'echo "$@" >> "$R2S_ARM_LOG"\n'
        'out=""\n'
        'prev=""\n'
        'for arg in "$@"; do\n'
        '    if [ "$prev" = "-o" ]; then out="$arg"; fi\n'
        '    prev="$arg"\n'
        'done\n'
        '[ -n "$out" ] && mkdir -p "$(dirname "$out")" && : > "$out"\n'
        'exit 0\n'
    )
    gcc.chmod(0o755)
    return {'R2S_ARM_LOG': str(log)}


def test_stage_only_stages_the_firmware_sources_and_reports_the_flags(tmp_path):
    out = tmp_path / 'out'
    result = run(['bash', str(SCRIPT), '--out', str(out), '--stage-only'], ROOT, tmp_path)

    assert result.returncode == 0, result.stderr
    for name in (
        'firmware.c',
        'r2s_board.h',
        'r2s_bluepill.c',
        'stm32f103.ld',
        'r2s_generated.h',
        'r2s_generated.c',
    ):
        assert (out / 'stage' / name).is_file(), name
    assert '-DR2S_WITH_CODEC' in result.stdout
    assert 'R2S_MAX_FRAME=' in result.stdout


def test_the_bluepill_build_invokes_arm_gcc_and_links_an_elf(tmp_path):
    out = tmp_path / 'out'
    log = tmp_path / 'arm.log'
    bin_dir = tmp_path / 'bin'
    env = stub_arm(bin_dir, log)
    env['PATH'] = stub_path(bin_dir)
    result = run(['bash', str(SCRIPT), '--out', str(out)], ROOT, tmp_path, extra_env=env)

    assert result.returncode == 0, result.stderr
    assert (out / 'firmware.elf').is_file()
    invocation = log.read_text(encoding='utf-8')
    assert 'cortex-m3' in invocation
    assert 'stm32f103.ld' in invocation


def test_the_build_fails_when_the_toolchain_is_missing(tmp_path):
    out = tmp_path / 'out'
    env = {'PATH': '/usr/bin:/bin', 'R2S_ARM_GCC': str(tmp_path / 'no-gcc')}
    result = run(['bash', str(SCRIPT), '--out', str(out)], ROOT, tmp_path, extra_env=env)

    assert result.returncode != 0
    assert 'arm-none-eabi-gcc' in result.stderr
