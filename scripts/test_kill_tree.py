"""kill-tree.sh TERMs a wrapper and the children it would orphan.

A bare kill of a launch wrapper leaves its node child alive, announcing
dead topics and reopening the port while the next leg runs. These tests
pin that mechanism and the helper that removes it, with sleep stand-ins
for the wrapper and the node: no ROS, no bridge, no timing dependence
beyond generous reap windows.
"""

import os
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HELPER = ROOT / 'scripts' / 'kill-tree.sh'


def run_helper(*args):
    """Run the helper with a clean environment."""
    return subprocess.run(
        ['bash', str(HELPER), *args],
        capture_output=True,
        text=True,
        env={'PATH': os.environ['PATH']},
        timeout=60,
    )


def start_orphaning_wrapper():
    """A wrapper that dies on TERM the way the launch wrapper does.

    The bash wrapper holds one sleeping child and forwards nothing, so
    a bare kill orphans the child exactly like the killed bridge leg.
    Returns the wrapper popen and the child's pid.
    """
    wrapper = subprocess.Popen(
        ['bash', '-c', 'sleep 120 & echo $!; wait'],
        stdout=subprocess.PIPE,
        text=True,
    )
    kid = int(wrapper.stdout.readline().strip())
    return wrapper, kid


def reap(wrapper):
    """Collect the wrapper without hanging when it is already gone."""
    try:
        wrapper.wait(timeout=10)
    except subprocess.TimeoutExpired:
        wrapper.kill()
        wrapper.wait(timeout=10)


def gone(pid):
    """A pid is gone when /proc lost it or only a zombie remains."""
    try:
        with open(f'/proc/{pid}/stat') as handle:
            fields = handle.read().rsplit(')', 1)[1].split()
    except FileNotFoundError:
        return True
    return fields[0] == 'Z'


def wait_gone(pid, timeout=10.0):
    """Poll until the pid is gone, so init gets its reap window."""
    deadline = time.monotonic() + timeout
    while not gone(pid):
        assert time.monotonic() < deadline, f'pid {pid} never died'
        time.sleep(0.1)


def cleanup(*pids):
    """Best-effort SIGKILL for leftover stand-ins."""
    for pid in pids:
        try:
            os.kill(pid, 9)
        except (ProcessLookupError, PermissionError):
            pass


def test_a_bare_kill_orphans_the_child():
    """The mechanism behind the flake: TERM kills the wrapper only."""
    wrapper, kid = start_orphaning_wrapper()
    try:
        wrapper.terminate()
        reap(wrapper)
        assert gone(wrapper.pid), 'the wrapper survived TERM'
        assert not gone(kid), 'the child died with the wrapper'
    finally:
        cleanup(kid)
        reap(wrapper)


def test_kill_tree_kills_the_wrapper_and_the_orphan():
    """The helper leaves neither the wrapper nor its child behind."""
    wrapper, kid = start_orphaning_wrapper()
    try:
        result = run_helper(str(wrapper.pid))
        assert result.returncode == 0, result.stderr
        wait_gone(wrapper.pid)
        wait_gone(kid)
    finally:
        cleanup(wrapper.pid, kid)
        reap(wrapper)


def live_marked(marker):
    """Pids whose command line carries the marker, zombies excluded."""
    result = subprocess.run(['pgrep', '-f', marker], capture_output=True, text=True)
    return [pid for pid in result.stdout.split() if not gone(pid)]


def test_kill_tree_reaches_grandchildren():
    """A wrapper whose child spawned its own child: all three die."""
    marker = f'r2s504-gc-{os.getpid()}'
    middle = f'exec -a {marker}-mid bash -c "exec -a {marker}-leaf sleep 120 & wait"'
    wrapper = subprocess.Popen(
        ['bash', '-c', f'{middle} & wait'],
        stdout=subprocess.DEVNULL,
        text=True,
    )
    try:
        deadline = time.monotonic() + 10.0
        while len(live_marked(marker)) < 3:
            assert time.monotonic() < deadline, 'the grandchild never started'
            time.sleep(0.1)
        result = run_helper(str(wrapper.pid))
        assert result.returncode == 0, result.stderr
        wait_gone(wrapper.pid)
        deadline = time.monotonic() + 10.0
        while live_marked(marker):
            assert time.monotonic() < deadline, 'the grandchild survived'
            time.sleep(0.1)
    finally:
        subprocess.run(['pkill', '-f', marker])
        cleanup(wrapper.pid)
        reap(wrapper)


def test_kill_tree_sigkills_a_term_ignorer():
    """A child that traps TERM still dies, quickly, by SIGKILL."""
    ignorer = subprocess.Popen(['bash', '-c', "trap '' TERM; sleep 60"])
    try:
        start = time.monotonic()
        result = run_helper(str(ignorer.pid), '1')
        assert result.returncode == 0, result.stderr
        wait_gone(ignorer.pid)
        assert time.monotonic() - start < 10, 'the SIGKILL path took too long'
    finally:
        cleanup(ignorer.pid)
        reap(ignorer)


def test_kill_tree_refuses_pid_1():
    """Killing init's tree would take the machine down, so it fails fast."""
    result = run_helper('1')
    assert result.returncode != 0, 'pid 1 was accepted'


def test_kill_tree_accepts_an_already_dead_pid():
    """A reaped pid counts as dead: the helper is idempotent."""
    sleeper = subprocess.Popen(['sleep', '0.01'])
    sleeper.wait(timeout=10)
    result = run_helper(str(sleeper.pid))
    assert result.returncode == 0, result.stderr


def test_kill_tree_rejects_a_non_pid():
    """Usage errors exit 2 the way the other script helpers do."""
    result = run_helper('not-a-pid')
    assert result.returncode == 2, result.stderr
