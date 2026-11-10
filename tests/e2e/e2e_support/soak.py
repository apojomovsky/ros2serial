# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

"""The soak gate: exact wire counters and bounded memory growth.

A clean link drops nothing, so the /diagnostics wire counters must read
identical before and after the run, whatever the traffic counters do. Host
memory must plateau after warm-up: allocator arenas grow early, a leak grows
forever. Both checks take plain data, so the fixture tests prove them with
no two hour run.
"""

import os

# Counters a clean link never moves; traffic counters are absent by design.
EXACT_KEYS = (
    'resets',
    'mcu crc errors',
    'mcu rx overflows',
    'mcu dropped frames',
    'mcu error code 1',
    'mcu error code 2',
    'mcu error code 3',
    'mcu error code 4',
    'mcu error code 5',
    'mcu error code 6',
)

_TOPIC_SUFFIXES = (' seq gaps', ' dropped', ' publish failures', ' mcu errors')


def _is_topic_counter(key):
    return key.startswith('topic ') and key.endswith(_TOPIC_SUFFIXES)


def check_wire_counters(base, final, what='soak'):
    """The wire counters read identical in two /diagnostics samples.

    base and final map diagnostic keys to their string values. Traffic
    counters such as mcu rx frames grow with the run and are ignored; every
    drop, error, gap and reset counter must match exactly.
    """
    for key in EXACT_KEYS:
        assert base.get(key) == final.get(key), (
            f'{what}: {key} moved {base.get(key)} to {final.get(key)}'
        )
    for key, value in base.items():
        if _is_topic_counter(key):
            assert final.get(key) == value, f'{what}: {key} moved {value} to {final.get(key)}'


def check_memory_growth(samples, warmup_s, tolerance_kb, what):
    """RSS plateaus after warm-up: at most tolerance_kb above its baseline.

    samples holds (elapsed seconds, RSS kB) pairs in time order. The
    baseline is the last sample before warm-up ends, so early allocator
    growth never counts and any later climb does. Needs at least one sample
    on each side of the warm-up boundary.
    """
    assert samples, f'{what}: no memory samples'
    start = samples[0][0]
    baseline = None
    post = []
    for elapsed, rss in samples:
        if elapsed - start < warmup_s:
            baseline = rss
        else:
            post.append(rss)
    assert baseline is not None, f'{what}: no memory sample before warm-up ends'
    assert post, f'{what}: no memory samples after warm-up'
    growth = max(post) - baseline
    assert growth <= tolerance_kb, (
        f'{what}: RSS grew {growth:.0f} kB after warm-up (tolerance {tolerance_kb:.0f} kB)'
    )


def rss_kb(pid):
    """The resident set size of pid in kB, read from /proc."""
    with open(f'/proc/{pid}/status', encoding='utf-8') as handle:
        for line in handle:
            if line.startswith('VmRSS:'):
                return float(line.split()[1])
    raise AssertionError(f'no VmRSS for pid {pid}')


def _command(pid):
    try:
        with open(f'/proc/{pid}/cmdline', 'rb') as handle:
            return handle.read().replace(b'\0', b' ').decode('utf-8', 'replace')
    except OSError:
        return ''


def _parent(pid):
    try:
        with open(f'/proc/{pid}/stat', encoding='utf-8') as handle:
            return int(handle.read().rsplit(')', 1)[1].split()[1])
    except (OSError, IndexError, ValueError):
        return 0


def find_bridge_pids():
    """PIDs of r2s_bridge processes below this one.

    The match walks the parent chain, so two suites on one host never read
    each other's bridge the way the DDS domain already separates them.
    """
    mine = os.getpid()
    found = []
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        pid = int(entry)
        if pid == mine or 'r2s_bridge' not in _command(pid):
            continue
        parent = _parent(pid)
        while parent > 1:
            if parent == mine:
                found.append(pid)
                break
            parent = _parent(parent)
    return sorted(found)
