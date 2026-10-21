"""cliff.toml groups commits by type, with breaking changes in their own section."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def changelog(tmp_path: Path, subjects: list[str]) -> str:
    repo = tmp_path / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "t")
    for i, subject in enumerate(subjects):
        (repo / f"f{i}").write_text(subject)
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", subject)
    out = subprocess.run(
        ["git-cliff", "--config", str(ROOT / "cliff.toml"), "--unreleased"],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
    )
    return out.stdout


def test_sections_by_type_and_breaking_first(tmp_path):
    text = changelog(
        tmp_path,
        [
            "feat(core): add the rCOBS encoder",
            "fix(bridge): drop long frames",
            "feat(spec)!: widen topic IDs",
            "docs: explain the time sync",
        ],
    )
    assert "### Breaking" in text
    assert "### Features" in text
    assert "### Fixes" in text
    assert "### Documentation" in text
    assert text.index("### Breaking") < text.index("### Features")
    breaking = text[text.index("### Breaking") : text.index("### Features")]
    assert "Widen topic IDs" in breaking
    assert "Add the rCOBS encoder" not in breaking


def test_unconventional_commits_are_left_out(tmp_path):
    text = changelog(tmp_path, ["feat(core): add the encoder", "Merge pull request #1 from someone/branch"])
    assert "Merge pull request" not in text
