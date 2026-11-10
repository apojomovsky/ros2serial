# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The host-wide gate caps how many suites run at once across trees.

The per-tree lock stops a second run of one tree; the host gate stops
several trees from running full suites at once on a shared host. Each
suite holds one of R2S_SUITE_SLOTS slots under the shared cache, so two
trees never run side by side once every slot is taken. A suite that finds
no free slot waits up to R2S_SUITE_HOST_WAIT_SECS and then stops with a
clear message instead of hanging; R2S_LOCK_NOWAIT=1 refuses at once.
"""

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCOPES = frozenset(
    [
        'LINT',
        'SCRIPTS',
        'COMMITS',
        'CORE',
        'GEN',
        'BRIDGE',
        'AMENT_LINT',
        'CPP',
        'E2E',
        'COVERAGE',
        'DOCS',
        'EXAMPLES',
        'ESP32',
        'PIC',
        'PLATFORMIO',
        'BUDGETS',
        'FUZZ',
        'README',
        'NIGHTLY',
        'SOAK',
        'SCRIPTS_SHARD',
        'TEST',
        'R2S_FULL',
    ]
)


def minimal_suite(dest: Path) -> Path:
    """A tree with the suite and the scripts it sources, running LINT only."""
    (dest / 'scripts').mkdir(parents=True)
    for name in ('ci-test.sh', 'clean-git-env.sh', 'suite-lock.sh'):
        shutil.copy2(ROOT / 'scripts' / name, dest / 'scripts' / name)
    return dest


def suite_env(
    cache: Path,
    *,
    slots: int | None = None,
    wait_secs: int | None = None,
    nowait: bool = False,
) -> dict:
    env = {k: v for k, v in os.environ.items() if k not in SCOPES}
    # A nested suite run inherits its ancestor's slot and skips the gate;
    # the fixtures below simulate top-level pushes, so they drop the marker.
    env.pop('R2S_SUITE_HOST_SLOT', None)
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), env.get('PATH', '')])
    env['LINT'] = '1'
    env['R2S_CACHE_DIR'] = str(cache)
    if slots is not None:
        env['R2S_SUITE_SLOTS'] = str(slots)
    if wait_secs is not None:
        env['R2S_SUITE_HOST_WAIT_SECS'] = str(wait_secs)
    if nowait:
        env['R2S_LOCK_NOWAIT'] = '1'
    return env


def hold_slot(cache: Path, index: int = 0, seconds: int = 30) -> subprocess.Popen:
    """Hold one gate slot the way a running suite does: flock while it sleeps."""
    slot = cache / 'suite-host-slots' / f'slot-{index}.lock'
    slot.parent.mkdir(parents=True, exist_ok=True)
    slot.touch()
    return subprocess.Popen(
        ['flock', str(slot), 'sleep', str(seconds)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def run_suite(tree: Path, env: dict) -> subprocess.CompletedProcess:
    return subprocess.run(
        ['bash', 'scripts/ci-test.sh'],
        cwd=tree,
        capture_output=True,
        text=True,
        env=env,
    )


def test_second_tree_uses_a_free_slot_under_the_cap(tmp_path):
    """With two slots and one held, a second tree still runs: the cap counts."""
    cache = tmp_path / 'cache'
    tree = minimal_suite(tmp_path / 'tree-b')
    holder = hold_slot(cache, seconds=30)
    try:
        time.sleep(0.5)
        out = run_suite(tree, suite_env(cache, slots=2))
    finally:
        holder.kill()
        holder.wait()
    assert out.returncode != 75, out.stdout + out.stderr
    assert 'host suite gate' not in out.stderr


def test_second_tree_waits_for_the_host_gate(tmp_path):
    """With one slot held, a second tree waits, then stops with a clear message."""
    cache = tmp_path / 'cache'
    tree = minimal_suite(tmp_path / 'tree-b')
    holder = hold_slot(cache, seconds=30)
    try:
        time.sleep(0.5)
        start = time.monotonic()
        out = run_suite(tree, suite_env(cache, slots=1, wait_secs=3))
        elapsed = time.monotonic() - start
    finally:
        holder.kill()
        holder.wait()
    assert out.returncode == 75, out.stdout + out.stderr
    assert elapsed >= 2.5, f'refused without waiting ({elapsed:.1f}s)'
    assert elapsed < 20, f'wait ran past its bound ({elapsed:.1f}s)'
    assert 'host suite gate' in out.stderr


def test_host_gate_refuses_at_once_when_asked_not_to_wait(tmp_path):
    """R2S_LOCK_NOWAIT=1 fails fast on a full gate instead of queueing."""
    cache = tmp_path / 'cache'
    tree = minimal_suite(tmp_path / 'tree-b')
    holder = hold_slot(cache, seconds=30)
    try:
        time.sleep(0.5)
        start = time.monotonic()
        out = run_suite(tree, suite_env(cache, slots=1, nowait=True))
        elapsed = time.monotonic() - start
    finally:
        holder.kill()
        holder.wait()
    assert out.returncode == 75, out.stdout + out.stderr
    assert elapsed < 10, f'nowait run queued ({elapsed:.1f}s)'
    assert 'host suite gate' in out.stderr


def test_bad_gate_knobs_fail_with_a_clear_message(tmp_path):
    """A nonsense cap or wait stops at once instead of running unlocked."""
    cache = tmp_path / 'cache'
    tree = minimal_suite(tmp_path / 'tree-b')
    out = run_suite(tree, suite_env(cache, slots=0))
    assert out.returncode == 2, out.stdout + out.stderr
    assert 'R2S_SUITE_SLOTS' in out.stderr
    env = suite_env(cache)
    env['R2S_SUITE_HOST_WAIT_SECS'] = 'soon'
    out = run_suite(tree, env)
    assert out.returncode == 2, out.stdout + out.stderr
    assert 'R2S_SUITE_HOST_WAIT_SECS' in out.stderr


def test_queued_suite_runs_once_a_slot_frees(tmp_path):
    """A waiting suite takes the slot its holder releases and runs on."""
    cache = tmp_path / 'cache'
    tree = minimal_suite(tmp_path / 'tree-b')
    holder = hold_slot(cache, seconds=2)
    try:
        out = run_suite(tree, suite_env(cache, slots=1, wait_secs=30))
    finally:
        holder.kill()
        holder.wait()
    assert out.returncode != 75, out.stdout + out.stderr
    assert 'stayed full' not in out.stderr
