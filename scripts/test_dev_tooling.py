"""The make entry points and their helpers: image tag, doctor, main-clone guard, build lock."""

import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(args, cwd=ROOT, env=None, check=False):
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, check=check)


def copy_tag_inputs(dest: Path) -> Path:
    for name in ["Dockerfile", "ci/versions.env", "scripts/dev-image-tag.sh"]:
        (dest / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, dest / name)
    return dest


def tag_of(root: Path) -> str:
    return run(["bash", "scripts/dev-image-tag.sh"], cwd=root, check=True).stdout.strip()


def test_image_tag_is_deterministic_and_content_addressed(tmp_path):
    root = copy_tag_inputs(tmp_path)
    first = tag_of(root)
    assert re.fullmatch(r"ros2serial-dev:[0-9a-f]{12}", first), first
    assert tag_of(root) == first
    (root / "Dockerfile").write_text((root / "Dockerfile").read_text() + "\n# changed\n")
    second = tag_of(root)
    assert second != first
    (root / "ci/versions.env").write_text((root / "ci/versions.env").read_text() + "X=1\n")
    assert tag_of(root) != second


def fake_bin(tmp_path: Path, names: list[str]) -> Path:
    bindir = tmp_path / "bin"
    bindir.mkdir()
    for name in names:
        real = shutil.which(name)
        (bindir / name).symlink_to(real)
    return bindir


def test_doctor_names_a_missing_dependency(tmp_path):
    bindir = fake_bin(tmp_path, ["bash", "git", "make", "sha256sum", "grep", "cat", "cut", "dirname", "id"])
    result = run(["bash", "scripts/doctor.sh"], env={"PATH": str(bindir), "HOME": str(tmp_path)})
    assert result.returncode != 0
    assert "docker" in result.stdout


def make_repo_with_worktree(tmp_path: Path) -> Path:
    repo = tmp_path / "repo"
    repo.mkdir()
    for args in (
        ["init", "-q", "-b", "master"],
        ["config", "user.email", "t@example.com"],
        ["config", "user.name", "t"],
        ["commit", "-q", "--allow-empty", "-m", "chore: start"],
        ["worktree", "add", "-q", ".worktrees/feat-1", "-b", "feat/1-x"],
    ):
        run(["git", *args], cwd=repo, check=True)
    return repo


def test_guard_refuses_the_main_clone_while_worktrees_exist(tmp_path):
    repo = make_repo_with_worktree(tmp_path)
    guard = str(ROOT / "scripts/check-main-clone-build.sh")
    env = {k: v for k, v in os.environ.items() if k != "ALLOW_MAIN_CLONE"}
    refused = run(["bash", guard], cwd=repo, env=env)
    assert refused.returncode != 0
    assert ".worktrees" in refused.stdout
    assert run(["bash", guard], cwd=repo / ".worktrees/feat-1", env=env).returncode == 0
    assert run(["bash", guard], cwd=repo, env={**env, "ALLOW_MAIN_CLONE": "1"}).returncode == 0


def test_guard_allows_the_main_clone_without_worktrees(tmp_path):
    repo = tmp_path / "solo"
    repo.mkdir()
    run(["git", "init", "-q", "-b", "master"], cwd=repo, check=True)
    assert run(["bash", str(ROOT / "scripts/check-main-clone-build.sh")], cwd=repo).returncode == 0


def test_build_lock_serializes_commands(tmp_path):
    """Two runs of one build directory take turns."""
    lock, log = tmp_path / "build.lock", tmp_path / "log"
    body = f"echo start >> {log}; sleep 0.3; echo end >> {log}"
    script = str(ROOT / "scripts/with-build-lock.sh")

    def one(env=None):
        return run(["bash", script, str(lock), "bash", "-c", body], env=env)

    threads = [threading.Thread(target=one) for _ in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert log.read_text().split() == ["start", "end", "start", "end"]


def test_build_lock_refuses_when_asked_not_to_wait(tmp_path):
    """Asked not to wait, a run that finds the lock held stops."""
    lock, log = tmp_path / "build.lock", tmp_path / "log"
    script = str(ROOT / "scripts/with-build-lock.sh")
    holder = subprocess.Popen(
        ["bash", script, str(lock), "bash", "-c", f"echo start >> {log}; sleep 5"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.5)
        env = {**os.environ, "R2S_LOCK_NOWAIT": "1"}
        out = run(["bash", script, str(lock), "bash", "-c", "true"], env=env)
        assert out.returncode == 75
        assert "busy" in out.stderr
        assert str(lock) in out.stderr
    finally:
        holder.kill()
        holder.wait()


def agents_targets() -> set[str]:
    text = (ROOT / "AGENTS.md").read_text()
    table = text[text.index("| Target | What |") : text.index("## Picking up work")]
    names = set()
    for line in (ln for ln in table.splitlines()[2:] if ln.startswith("|")):
        # Cells split on unescaped pipes: a target cell may contain `uno\|mega`.
        first = re.split(r"(?<!\\)\|", line)[1]
        names.update(re.findall(r"`([a-z][a-z-]*)", first))
    return names - {"Target"}


def test_make_help_lists_exactly_the_targets_in_agents_md():
    out = run(["make", "help"], check=True).stdout
    listed = set(re.findall(r"^  ([a-z][a-z-]*)\s", out, re.MULTILINE))
    assert listed == agents_targets()


def test_make_test_reaches_the_suite_script_in_and_out_of_the_image():
    inside = run(["make", "-n", "test", "R2S_IN_IMAGE=1"], check=True).stdout
    assert "bash scripts/ci-test.sh" in inside
    assert "docker" not in inside
    outside = run(["make", "-n", "test", "R2S_IN_IMAGE="], check=True).stdout
    assert re.search(r"docker run .* bash scripts/ci-test\.sh", outside), outside


def test_the_suite_refuses_a_second_run_on_the_same_tree(tmp_path):
    """Two suite runs at once would test a tree the first one changed.

    The run happens in a copy of the tree, so the suite it starts does not
    run this test again.
    """
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "scripts").mkdir()
    shutil.copy2(ROOT / "scripts/ci-test.sh", tree / "scripts/ci-test.sh")
    lock = tmp_path / "suite.lock"
    env = {**os.environ, "R2S_SUITE_LOCK": str(lock), "LINT": "1"}
    holder = subprocess.Popen(
        ["bash", str(ROOT / "scripts/with-build-lock.sh"), str(lock), "sleep", "5"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.5)
        out = run(["bash", "scripts/ci-test.sh"], cwd=tree, env=env)
        assert out.returncode == 75, out.stdout + out.stderr
        assert "already running" in out.stderr
    finally:
        holder.kill()
        holder.wait()


def test_the_suite_queues_when_asked(tmp_path):
    """Asked to wait, the second run waits and then runs."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "scripts").mkdir()
    shutil.copy2(ROOT / "scripts/ci-test.sh", tree / "scripts/ci-test.sh")
    lock = tmp_path / "suite.lock"
    env = {**os.environ, "R2S_SUITE_LOCK": str(lock), "LINT": "1", "R2S_SUITE_LOCK_WAIT": "1"}
    holder = subprocess.Popen(
        ["bash", str(ROOT / "scripts/with-build-lock.sh"), str(lock), "sleep", "2"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.3)
        out = run(["bash", "scripts/ci-test.sh"], cwd=tree, env=env)
        assert "already running" not in out.stderr, out.stdout + out.stderr
    finally:
        holder.kill()
        holder.wait()
