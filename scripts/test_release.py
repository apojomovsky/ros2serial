"""The release cut: VERSION is the single source, and the next version follows the commits."""

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def repo_with(tmp_path: Path, tag: str, subjects: list[str]) -> Path:
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    (repo / "VERSION").write_text(tag.removeprefix("v") + "\n")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "chore: start")
    git(repo, "tag", tag)
    for i, s in enumerate(subjects):
        (repo / f"f{i}").write_text(s)
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", s)
    return repo


def next_version(repo: Path) -> str:
    out = subprocess.run(
        ["bash", str(ROOT / "scripts/release.sh"), "--dry-run"], cwd=repo, capture_output=True, text=True, check=True
    )
    m = re.search(r"next version: (\S+)", out.stdout)
    assert m, out.stdout
    return m.group(1)


def test_version_file_is_semver():
    assert re.fullmatch(r"\d+\.\d+\.\d+\n", (ROOT / "VERSION").read_text())


def test_fix_bumps_patch(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["fix(core): a"])) == "0.3.2"


def test_feature_bumps_minor(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["fix(core): a", "feat(core): b"])) == "0.4.0"


def test_breaking_bumps_minor_before_one_point_zero(tmp_path):
    assert next_version(repo_with(tmp_path, "v0.3.1", ["feat(spec)!: c"])) == "0.4.0"


def test_breaking_bumps_major_after_one_point_zero(tmp_path):
    assert next_version(repo_with(tmp_path, "v1.2.0", ["feat(spec)!: c"])) == "2.0.0"


def test_dry_run_changes_nothing(tmp_path):
    repo = repo_with(tmp_path, "v0.3.1", ["feat(core): b"])
    next_version(repo)
    assert (repo / "VERSION").read_text() == "0.3.1\n"
    assert not (repo / "CHANGELOG.md").exists()
