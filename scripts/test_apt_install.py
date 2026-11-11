# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""scripts/apt-install.sh retries transient apt failures, then fails loud.

The dev image, the rosdoc2 job, the farm job and the e2e-posix legs all
install through this helper, so one transient index failure costs seconds
instead of a whole rerun. These tests drive the helper with a stub apt-get
on PATH: no network, no root, no real package lists.
"""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / 'scripts' / 'apt-install.sh'

STUB = """#!/usr/bin/env bash
# A stub apt-get: counts its runs, records its argv, then fails until
# the count passes FAIL_UNTIL. Runs up to DIRTY_UNTIL print FAIL_LINE
# but still exit 0, like an update that ignores every mirror.
# Every line it prints is the "apt output" the helper must match
# or surface.
count=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" >"$COUNT_FILE"
printf '%s\\n' "$*" >>"$ARGV_LOG"
if [ "$count" -le "${DIRTY_UNTIL:-0}" ]; then
    echo "$FAIL_LINE (attempt $count)"
    exit 0
fi
if [ "$count" -le "$FAIL_UNTIL" ]; then
    echo "$FAIL_LINE (attempt $count)"
    exit "$FAIL_RC"
fi
echo "all good (attempt $count)"
exit 0
"""


def run_helper(bindir, stub_env, *args):
    """Run the helper with the stub bin dir first on PATH, no backoff."""
    env = dict(os.environ)
    env['PATH'] = str(bindir) + ':' + env['PATH']
    env['R2S_APT_RETRY_DELAY'] = '0'
    env.update(stub_env)
    return subprocess.run(
        ['bash', str(HELPER), *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )


def make_stub(tmp_path, fail_until, rc=100, line='boom', dirty_until=0):
    """A bin dir whose `apt-get` fails until past the fail_until-th run."""
    bindir = tmp_path / 'bin'
    bindir.mkdir()
    stub = bindir / 'apt-get'
    stub.write_text(STUB)
    stub.chmod(0o755)
    count = tmp_path / 'count'
    argv_log = tmp_path / 'argv'
    argv_log.write_text('')
    sources = tmp_path / 'apt-etc'
    (sources / 'sources.list.d').mkdir(parents=True, exist_ok=True)
    stub_env = {
        'COUNT_FILE': str(count),
        'ARGV_LOG': str(argv_log),
        'FAIL_UNTIL': str(fail_until),
        'FAIL_RC': str(rc),
        'FAIL_LINE': line,
        'DIRTY_UNTIL': str(dirty_until),
        'R2S_APT_SOURCES_DIR': str(sources),
    }
    return bindir, count, argv_log, stub_env


UBUNTU_SOURCES = """Types: deb
URIs: http://archive.ubuntu.com/ubuntu/
Suites: noble noble-updates
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://security.ubuntu.com/ubuntu/
Suites: noble-security
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
"""

ONE_LINE_SOURCES = 'deb http://archive.ubuntu.com/ubuntu/ noble main universe\n'

# A source that never mentions the Ubuntu mirrors: the fallback must leave
# it byte-identical.
OTHER_SOURCES = """Types: deb
URIs: http://packages.ros.org/ros2/ubuntu/
Suites: noble
Components: main
"""


def write_ubuntu_sources(tmp_path):
    """Fixture apt sources: deb822, one-line and an unrelated file."""
    sources = tmp_path / 'apt-etc'
    (sources / 'sources.list.d').mkdir(parents=True, exist_ok=True)
    (sources / 'sources.list.d' / 'ubuntu.sources').write_text(UBUNTU_SOURCES)
    (sources / 'sources.list').write_text(ONE_LINE_SOURCES)
    (sources / 'sources.list.d' / 'other.sources').write_text(OTHER_SOURCES)


def attempts(count_file):
    """How many times the stub apt-get ran."""
    return int(count_file.read_text().strip())


def test_transient_fetch_failure_is_retried_then_succeeds(tmp_path):
    """A `Failed to fetch` on the first run succeeds on the second."""
    bindir, count, argv_log, stub_env = make_stub(
        tmp_path,
        fail_until=1,
        line='E: Failed to fetch http://archive.ubuntu.com/ 500 Error',
    )
    result = run_helper(bindir, stub_env, 'install', '-y', 'some-package')
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 2, 'the transient failure was not retried'
    assert 'Acquire::Retries=3' in argv_log.read_text(), (
        'the helper must pass apt its own per-fetch retries'
    )
    assert 'Acquire::ForceIPv4=true' in argv_log.read_text(), (
        'the helper must force IPv4: the runners have no working IPv6 route'
    )


def test_persistent_failure_surfaces_the_real_error(tmp_path):
    """A package that never exists fails bounded, with apt's own error."""
    line = "E: Package 'gone' has no installation candidate"
    bindir, count, argv_log, stub_env = make_stub(tmp_path, fail_until=99, line=line)
    result = run_helper(bindir, stub_env, 'install', '-y', 'gone')
    assert result.returncode != 0, 'a missing package reported success'
    assert attempts(count) == 5, 'the attempts were not bounded at 5'
    assert line in result.stdout + result.stderr, 'the last apt error never reached the log'
    assert 'rerun the job' in result.stderr, 'the give-up must name the remedy'


def test_resolve_failure_swaps_in_the_azure_mirror(tmp_path):
    """A `Could not resolve` rewrites Ubuntu sources, then the retry runs."""
    write_ubuntu_sources(tmp_path)
    line = 'W: Failed to fetch http://archive.ubuntu.com/ubuntu/ '
    line += "Could not resolve 'archive.ubuntu.com'"
    bindir, count, argv_log, stub_env = make_stub(tmp_path, fail_until=1, line=line)
    result = run_helper(bindir, stub_env, 'update')
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 2, 'the retry after the swap never ran'
    sources = tmp_path / 'apt-etc'
    ubuntu = (sources / 'sources.list.d' / 'ubuntu.sources').read_text()
    assert 'azure.archive.ubuntu.com' in ubuntu, 'the deb822 sources kept the dead host'
    assert 'http://archive.ubuntu.com' not in ubuntu, 'the old archive host survived'
    assert 'http://security.ubuntu.com' not in ubuntu, 'the old security host survived'
    one_line = (sources / 'sources.list').read_text()
    assert 'azure.archive.ubuntu.com' in one_line, 'the one-line sources kept the dead host'
    assert 'http://archive.ubuntu.com' not in one_line
    other = (sources / 'sources.list.d' / 'other.sources').read_text()
    assert other == OTHER_SOURCES, 'a source without Ubuntu mirrors was rewritten'
    assert 'azure.archive.ubuntu.com' in result.stderr, 'the swap was not announced'


def test_no_resolve_failure_leaves_sources_untouched(tmp_path):
    """Without a resolve failure the sources stay byte-identical."""
    write_ubuntu_sources(tmp_path)
    bindir, count, argv_log, stub_env = make_stub(tmp_path, fail_until=99, line='E: boom')
    result = run_helper(bindir, stub_env, 'update')
    assert result.returncode != 0, 'a failing command reported success'
    sources = tmp_path / 'apt-etc'
    assert (sources / 'sources.list.d' / 'ubuntu.sources').read_text() == UBUNTU_SOURCES
    assert (sources / 'sources.list').read_text() == ONE_LINE_SOURCES
    assert (sources / 'sources.list.d' / 'other.sources').read_text() == OTHER_SOURCES


def test_update_that_ignores_every_mirror_is_retried_after_swap(tmp_path):
    """An update exiting 0 with failed fetches swaps mirrors and reruns."""
    write_ubuntu_sources(tmp_path)
    line = 'W: Failed to fetch http://archive.ubuntu.com/ubuntu/ '
    line += "Could not resolve 'archive.ubuntu.com'"
    bindir, count, argv_log, stub_env = make_stub(tmp_path, fail_until=0, line=line, dirty_until=1)
    result = run_helper(bindir, stub_env, 'update')
    assert result.returncode == 0, result.stderr
    assert attempts(count) == 2, 'the ignored update was reported as success'
    ubuntu = (tmp_path / 'apt-etc' / 'sources.list.d' / 'ubuntu.sources').read_text()
    assert 'azure.archive.ubuntu.com' in ubuntu, 'the swap never ran'
    assert 'azure.archive.ubuntu.com' in result.stderr, 'the swap was not announced'


def test_missing_subcommand_is_a_usage_error(tmp_path):
    """No apt subcommand exits 2 the way the other script helpers do."""
    bindir, _, _, stub_env = make_stub(tmp_path, fail_until=0)
    result = run_helper(bindir, stub_env)
    assert result.returncode == 2, result.stderr


APT_CALL_SITES = (
    'Dockerfile',
    'docker/bigendian/Dockerfile',
    'docker/mutation/Dockerfile',
    '.github/workflows/ci.yml',
    '.github/workflows/farm.yml',
    '.github/workflows/rosdoc2.yml',
)


def test_every_apt_call_site_uses_the_shared_helper():
    """No bare apt-get remains where the helper should run instead."""
    for site in APT_CALL_SITES:
        text = (ROOT / site).read_text(encoding='utf-8')
        assert 'apt-install.sh' in text, f'{site} bypasses the shared helper'
    offenders = []
    for site in APT_CALL_SITES:
        lines = (ROOT / site).read_text(encoding='utf-8').splitlines()
        for number, line in enumerate(lines, 1):
            if 'apt-get' in line and 'apt-install' not in line:
                offenders.append(f'{site}:{number}: {line.strip()}')
    assert not offenders, 'bare apt-get calls the helper must cover:\n' + '\n'.join(offenders)
