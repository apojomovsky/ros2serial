"""suite-lock.sh names the lock a suite run of a tree takes.

One tree keeps one lock, so a second suite refuses it. Two trees never share
one, whether they are two worktrees on the host or two temporary copies that
happen to sit at the same path in their own containers.
"""

import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def lock_path(
    tree: Path,
    home: Path,
    *,
    mount: Path | None = None,
    cache: Path | None = None,
    build: Path | None = None,
    container_tmp: Path | None = None,
) -> Path:
    env = {"PATH": os.environ["PATH"], "HOME": str(home)}
    if mount is not None:
        env["R2S_HOST_MOUNT"] = str(mount)
    if cache is not None:
        env["R2S_CACHE_DIR"] = str(cache)
    if build is not None:
        env["R2S_BUILD_DIR"] = str(build)
    if container_tmp is not None:
        env["TMPDIR"] = str(container_tmp)
    result = subprocess.run(
        ["bash", str(ROOT / "scripts" / "suite-lock.sh"), str(tree)],
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stderr
    return Path(result.stdout.strip())


def test_one_tree_keeps_one_lock(tmp_path):
    tree = tmp_path / "host" / "tree"
    assert lock_path(tree, tmp_path) == lock_path(tree, tmp_path)


def test_two_trees_never_share_a_lock(tmp_path):
    """A "-" for "/" swap alone maps these two trees to one lock name."""
    assert lock_path(tmp_path / "a" / "b", tmp_path) != lock_path(tmp_path / "a-b", tmp_path)


def test_the_tree_the_image_mounts_locks_in_the_cache_for_every_container(tmp_path):
    mount = tmp_path / "host"
    cache = tmp_path / "cache"
    tree = mount / "tree"
    first = lock_path(tree, tmp_path, mount=mount, cache=cache, container_tmp=tmp_path / "one" / "tmp")
    second = lock_path(tree, tmp_path, mount=mount, cache=cache, container_tmp=tmp_path / "two" / "tmp")
    assert first == second
    assert first.is_relative_to(cache)


def test_a_copy_made_inside_a_container_locks_in_that_container(tmp_path):
    """pytest's copy sits at the same path in every container.

    Both copies are of one tree, so they must not refuse each other: the lock
    lives in the container's own tmp, not in the cache the containers share.
    """
    mount = tmp_path / "host"
    cache = tmp_path / "cache"
    copy = Path("/tmp/pytest-of-ubuntu/pytest-0/test0")
    first = lock_path(copy, tmp_path, mount=mount, cache=cache, container_tmp=tmp_path / "one" / "tmp")
    second = lock_path(copy, tmp_path, mount=mount, cache=cache, container_tmp=tmp_path / "two" / "tmp")
    assert first != second
    assert first.is_relative_to(tmp_path / "one" / "tmp")
    assert second.is_relative_to(tmp_path / "two" / "tmp")


def test_one_copy_keeps_one_lock_in_its_container(tmp_path):
    copy = Path("/tmp/pytest-of-ubuntu/pytest-0/test0")
    container = tmp_path / "one" / "tmp"
    first = lock_path(copy, tmp_path, mount=tmp_path / "host", container_tmp=container)
    second = lock_path(copy, tmp_path, mount=tmp_path / "host", container_tmp=container)
    assert first == second


def test_without_a_mount_the_cache_holds_every_lock(tmp_path):
    """Not knowing what the image mounts, a shared lock is the safe mistake."""
    tree = Path("/tmp/pytest-of-ubuntu/pytest-0/test0")
    lock = lock_path(tree, tmp_path, cache=tmp_path / "cache", container_tmp=tmp_path / "one" / "tmp")
    assert lock.is_relative_to(tmp_path / "cache")


def test_the_cache_follows_the_build_directory(tmp_path):
    build = tmp_path / "build-root" / "build-tree"
    lock = lock_path(tmp_path / "a" / "b", tmp_path, build=build)
    assert lock.is_relative_to(tmp_path / "build-root")
