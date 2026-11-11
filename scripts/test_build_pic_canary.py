# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

import yaml

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'build-pic-canary.sh'
VERSIONS = ROOT / 'ci' / 'versions.env'
BASELINE = ROOT / 'ci' / 'pic-canary-baseline.txt'

TARGETS = ('p16f1938', 'p18f4550')
LABELS = ('core C sources', 'static C++ sketch')

# The recorded epic-cc failures the baseline pins, per target.
KNOWN_SIGS = {
    'p16f1938': 'isel: plain ptr arg with multiple terms not yet supported',
    'p18f4550': 'legalize: intrinsic llvm.memset.p0.i16 must carry a dst',
}


def target_aware_stub(path: Path, message_for_target) -> Path:
    """A fake epic-cc that fails each compile with its target's message."""
    path.mkdir(parents=True, exist_ok=True)
    compiler = path / 'epic-cc'
    lines = [
        '#!/usr/bin/env bash',
        'target=""',
        'prev=""',
        'for arg in "$@"; do',
        '    if [ "$prev" = "--target" ]; then target="$arg"; fi',
        '    prev="$arg"',
        'done',
        'case "$target" in',
    ]
    for target, message in message_for_target.items():
        lines += [f'    {target}) echo "{message}" >&2 ;;']
    lines += ['    *) echo "epic-cc: internal compiler error: unknown target" >&2 ;;']
    lines += ['esac', 'exit 1']
    compiler.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    compiler.chmod(compiler.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return compiler


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


def test_a_known_toolchain_gap_warns_and_stays_green(tmp_path):
    # The recorded epic-cc internal compiler errors are the toolchain's gap,
    # not the core's: the run warns, records the gap in the job summary, and
    # passes, so the advisory job stays green and the finding stays visible.
    compiler = target_aware_stub(
        tmp_path / 'bin',
        {target: f'epic-cc: internal compiler error: {KNOWN_SIGS[target]}' for target in TARGETS},
    )
    summary = tmp_path / 'summary.md'
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
            'GITHUB_STEP_SUMMARY': str(summary),
        },
        tmp_path,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert '::warning::pic-canary' in result.stdout, result.stdout
    body = summary.read_text(encoding='utf-8')
    for target in TARGETS:
        assert KNOWN_SIGS[target] in body, body
        assert target in body, body


def test_the_baseline_lists_each_known_gap(tmp_path):
    assert BASELINE.is_file(), 'ci/pic-canary-baseline.txt is missing'
    entries = [
        line.strip()
        for line in BASELINE.read_text(encoding='utf-8').splitlines()
        if line.strip() and not line.strip().startswith('#')
    ]
    assert len(entries) == len(TARGETS) * len(LABELS), entries
    for target in TARGETS:
        for label in LABELS:
            match = [line for line in entries if line.startswith(f'{target} | {label} | ')]
            assert len(match) == 1, f'no baseline line for {target} ({label}): {entries}'
            assert KNOWN_SIGS[target] in match[0], match


def test_a_new_error_signature_fails_and_names_target_label_signature(tmp_path):
    # A compiler message the baseline never recorded is a new break: the
    # canary fails and names the target, the label and the signature.
    compiler = target_aware_stub(
        tmp_path / 'bin',
        {
            'p16f1938': 'epic-cc: internal compiler error: isel: plain ptr arg with multiple terms not yet supported',
            'p18f4550': 'epic-cc: internal compiler error: regalloc: out of registers spilling i8',
        },
    )
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
            'GITHUB_STEP_SUMMARY': str(tmp_path / 'summary.md'),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    out = result.stdout + result.stderr
    assert 'p18f4550' in out, out
    assert 'regalloc: out of registers spilling i8' in out, out
    assert 'baseline' in out.lower(), out


def test_a_reworded_diagnostic_is_a_new_gap(tmp_path):
    # The match is strict about the message itself: a reworded diagnostic
    # never silently matches the recorded signature.
    compiler = target_aware_stub(
        tmp_path / 'bin',
        {
            'p16f1938': 'epic-cc: internal compiler error: isel: plain ptr arg with several terms not yet supported',
            'p18f4550': f'epic-cc: internal compiler error: {KNOWN_SIGS["p18f4550"]}',
        },
    )
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'several terms' in result.stdout + result.stderr


def test_line_number_churn_still_matches_the_baseline(tmp_path):
    # File and line prefixes move with every edit, so the match strips them
    # and only the message itself decides.
    compiler = target_aware_stub(
        tmp_path / 'bin',
        {
            target: (
                f'canary.c:{123 + i}:4: epic-cc: internal compiler error: '
                f'{KNOWN_SIGS[target]} [at line {456 + i}]'
            )
            for i, target in enumerate(TARGETS)
        },
    )
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
        },
        tmp_path,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_stale_baseline_entry_fails_and_is_reported(tmp_path):
    # A baseline entry that stops reproducing is news: the run reports it
    # instead of keeping the improvement hidden.
    baseline = tmp_path / 'baseline.txt'
    baseline.write_text(
        'p16f1938 | core C sources | isel: no longer broken\n'
        'p16f1938 | static C++ sketch | isel: no longer broken\n'
        'p18f4550 | core C sources | legalize: no longer broken\n'
        'p18f4550 | static C++ sketch | legalize: no longer broken\n',
        encoding='utf-8',
    )
    compiler = stub_compiler(tmp_path / 'bin')
    summary = tmp_path / 'summary.md'
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
            'R2S_PIC_CANARY_BASELINE': str(baseline),
            'GITHUB_STEP_SUMMARY': str(summary),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    out = result.stdout + result.stderr + summary.read_text(encoding='utf-8')
    assert 'no longer broken' in out, out


def test_a_missing_baseline_fails_the_run(tmp_path):
    # A baseline the script cannot read is wiring, not a gap, so it fails.
    compiler = stub_compiler(tmp_path / 'bin')
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
            'R2S_PIC_CANARY_BASELINE': str(tmp_path / 'no-such-baseline.txt'),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr


def test_the_failure_says_how_to_update_the_baseline(tmp_path):
    compiler = target_aware_stub(
        tmp_path / 'bin',
        {
            target: 'epic-cc: internal compiler error: isel: brand new failure'
            for target in TARGETS
        },
    )
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(compiler),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    out = result.stdout + result.stderr
    assert 'ci/pic-canary-baseline.txt' in out, out


def test_ci_warns_on_pull_requests_and_fails_nightly_and_manual():
    workflow = yaml.safe_load((ROOT / '.github' / 'workflows' / 'ci.yml').read_text())
    job = workflow['jobs'].get('pic-canary')
    assert job is not None, 'ci.yml has no pic-canary job'
    assert job.get('continue-on-error') == "${{ github.event_name == 'pull_request' }}", (
        'the canary must stay advisory on pull requests and fail '
        'nightly and manual runs instead of staying green everywhere'
    )


def test_a_broken_override_fails_the_run(tmp_path):
    # An override naming no executable is a wiring failure, not a known gap.
    result = run_script(
        {
            'R2S_EPIC_CC_BIN': str(tmp_path / 'no-such-compiler'),
            'R2S_BUILD_DIR': str(tmp_path / 'build'),
        },
        tmp_path,
    )
    assert result.returncode != 0, result.stdout + result.stderr


def test_a_failed_download_fails_the_run(tmp_path):
    # curl runs but the fetch fails: the bundle is unreachable, which is a
    # wiring failure, unlike a toolchain the environment cannot provide.
    path = tmp_path / 'badload'
    path.mkdir()
    (path / 'curl').write_text('#!/usr/bin/env bash\nexit 1\n', encoding='utf-8')
    (path / 'curl').chmod(0o755)
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
    assert result.returncode != 0, result.stdout + result.stderr
    assert 'cannot fetch' in result.stdout + result.stderr


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
