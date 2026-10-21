"""The make entry points and their helpers: image tag, doctor, main-clone guard, build lock."""

import os
import re
import shutil
import subprocess
import threading
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
    lock, log = tmp_path / "build.lock", tmp_path / "log"
    body = f"echo start >> {log}; sleep 0.3; echo end >> {log}"

    def one():
        run(["bash", str(ROOT / "scripts/with-build-lock.sh"), str(lock), "bash", "-c", body], check=True)

    threads = [threading.Thread(target=one) for _ in range(2)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert log.read_text().split() == ["start", "end", "start", "end"]


def agents_targets() -> set[str]:
    text = (ROOT / "AGENTS.md").read_text()
    table = text[text.index("| Target | What |") : text.index("## Picking up work")]
    names = set()
    for cell in re.findall(r"^\| ([^|]+) \|", table, re.MULTILINE):
        names.update(re.findall(r"`([a-z][a-z-]*)", cell))
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
