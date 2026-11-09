"""The PIC canary: core and static C++ compile with epic-cc, advisory.

The epic-cc bundle lives outside the dev image: the build downloads the
version pinned in ci/versions.env into a cache directory (never /opt) and
compiles the core C sources plus a static C++ sketch for PIC16F1938 and
PIC18F4550. A run without the toolchain skips with a message instead of
failing, so the advisory job stays cheap.
"""

import os
import re
import stat
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-pic-canary.sh'
VERSIONS = ROOT / 'ci' / 'versions.env'

TARGETS = ('p16f1938', 'p18f4550')


def pinned(name):
    for line in VERSIONS.read_text(encoding='utf-8').splitlines():
        if line.startswith(name + '='):
            return line.split('=', 1)[1]
    raise AssertionError(f'ci/versions.env names no {name}')


def test_the_bundle_version_is_pinned_in_versions_env():
    assert pinned('EPIC_CC_VERSION').count('.') == 2, pinned('EPIC_CC_VERSION')


def test_the_bundle_hash_is_pinned_in_versions_env():
    digest = pinned('EPIC_CC_SHA256')
    assert re.fullmatch(r'[0-9a-f]{64}', digest), digest


def test_the_script_reads_both_pins_from_versions_env():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'EPIC_CC_VERSION' in text
    assert 'EPIC_CC_SHA256' in text
    assert 'ci/versions.env' in text


def test_the_script_compiles_both_canary_targets():
    text = SCRIPT.read_text(encoding='utf-8')
    for target in TARGETS:
        assert target in text, f'the canary names no {target}'


def test_the_script_builds_the_core_c_sources_without_the_test_reset():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'core/src' in text
    assert 'test_reset' in text
    assert 'r2s_bundled.c' in text


def test_the_script_builds_a_static_cpp_sketch():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'R2S_TOPICS' in text
    assert 'ros2serial.h' in text


def test_the_bundle_lands_in_the_cache_never_the_image():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'R2S_CACHE_DIR' in text
    assert 'epic-cc' in text
    assert '/opt' not in text


def test_the_download_is_verified_before_it_runs():
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'releases/download' in text
    assert 'sha256sum' in text


def test_the_bundled_resource_headers_rejoin_the_search_path():
    # epic-cc drives clang with -nostdinc, which also drops the compiler's
    # own headers; without them the core's float.h include cannot resolve.
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'clang/lib/clang/' in text


def test_the_extracted_driver_and_helpers_are_made_executable():
    # The release zip carries no permission bits, so the driver and its
    # bundled clang land non-executable and the compile fails with denied.
    text = SCRIPT.read_text(encoding='utf-8')
    assert 'chmod +x' in text
    assert 'clang/bin/' in text


def run_script(env_extra, tmp_path):
    env = {k: v for k, v in os.environ.items() if k != 'PIC'}
    env.update(env_extra)
    return subprocess.run(
        ['bash', str(SCRIPT)], cwd=ROOT, capture_output=True, text=True, env=env, timeout=300
    )


def stub_compiler(path: Path) -> Path:
    """A fake epic-cc: records its arguments and touches the -o output."""
    path.mkdir(parents=True, exist_ok=True)
    compiler = path / 'epic-cc'
    log = path / 'calls.log'
    compiler.write_text(
        '#!/usr/bin/env bash\n'
        f'echo "$@" >> "{log}"\n'
        'out=""\n'
        'prev=""\n'
        'for arg in "$@"; do\n'
        '    if [ "$prev" = "-o" ]; then out="$arg"; fi\n'
        '    prev="$arg"\n'
        'done\n'
        '[ -n "$out" ] && mkdir -p "$(dirname "$out")" && : > "$out"\n',
        encoding='utf-8',
    )
    compiler.chmod(compiler.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return compiler


def test_the_stubbed_toolchain_compiles_c_and_cpp_for_both_targets(tmp_path):
    build = tmp_path / 'build'
    compiler = stub_compiler(tmp_path / 'bin')
    result = run_script(
        {'R2S_EPIC_CC_BIN': str(compiler), 'R2S_BUILD_DIR': str(build)},
        tmp_path,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    calls = (tmp_path / 'bin' / 'calls.log').read_text(encoding='utf-8')
    for target in TARGETS:
        assert f'--target {target}' in calls, f'no compile for {target}:\n{calls}'
    assert '.cpp' in calls, f'no C++ translation unit compiled:\n{calls}'
    assert '.c' in calls, f'no C translation unit compiled:\n{calls}'


def test_a_failing_toolchain_fails_the_run(tmp_path):
    # A nonzero compiler exit (the recorded ICEs) fails the section: the
    # advisory job reports red instead of passing over a broken toolchain.
    stub = tmp_path / 'bin'
    stub.mkdir()
    compiler = stub / 'epic-cc'
    compiler.write_text('#!/usr/bin/env bash\nexit 1\n', encoding='utf-8')
    compiler.chmod(compiler.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    result = run_script(
        {'R2S_EPIC_CC_BIN': str(compiler), 'R2S_BUILD_DIR': str(tmp_path / 'build')},
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'pic-canary' in result.stdout + result.stderr


def test_the_run_without_a_toolchain_skips_instead_of_failing(tmp_path):
    # No override, an empty cache, and no downloader on PATH: the bundle
    # cannot be fetched, so the canary reports the skip and passes.
    path = tmp_path / 'nodeload'
    path.mkdir()
    for tool in (
        'bash',
        'env',
        'mkdir',
        'rm',
        'cp',
        'grep',
        'cut',
        'python3',
        'sha256sum',
        'uname',
        'dirname',
    ):
        source = Path('/usr/bin') / tool
        if not source.exists():
            source = Path('/bin') / tool
        if source.exists():
            (path / tool).symlink_to(source)
    env = {k: v for k, v in os.environ.items() if k not in ('PIC', 'R2S_EPIC_CC_BIN')}
    env.update(
        {
            'PATH': str(path),
            'R2S_CACHE_DIR': str(tmp_path / 'cache'),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
        }
    )
    result = subprocess.run(
        ['bash', str(SCRIPT)], cwd=ROOT, capture_output=True, text=True, env=env, timeout=300
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'skip' in result.stdout.lower(), result.stdout
